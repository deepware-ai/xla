/* Copyright 2021 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "xla/service/async_collective_creator.h"

#include <iterator>
#include <memory>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "xla/frontend_attributes.h"
#include "xla/hlo/ir/hlo_casting_utils.h"
#include "xla/hlo/ir/hlo_instruction.h"
#include "xla/hlo/ir/hlo_instructions.h"
#include "xla/hlo/ir/hlo_schedule.h"
#include "xla/service/shape_inference.h"
#include "tsl/platform/errors.h"

namespace xla {

StatusOr<bool> AsyncCollectiveCreator::Run(
    HloModule* module,
    const absl::flat_hash_set<absl::string_view>& execution_threads) {
  bool changed = false;
  struct ReplacedAsync {
    HloInstruction* start;
    HloInstruction* done;
  };
  for (HloComputation* computation :
       module->MakeNonfusionComputations(execution_threads)) {
    // Find all supported collective ops first as we can't modify the
    // instructions while iterating through them.
    std::vector<HloInstruction*> supported_collectives;
    for (HloInstruction* instruction : computation->instructions()) {
      if ((instruction->opcode() == HloOpcode::kAllReduce &&
           convert_all_reduce_(instruction)) ||
          (instruction->opcode() == HloOpcode::kAllGather &&
           convert_all_gather_(instruction)) ||
          (instruction->opcode() == HloOpcode::kCollectivePermute &&
           convert_collective_permute_(instruction)) ||
          (instruction->opcode() == HloOpcode::kAllToAll &&
           convert_all_to_all_(instruction))) {
        supported_collectives.push_back(instruction);
      }
    }
    if (supported_collectives.empty()) {
      continue;
    }

    absl::flat_hash_map<HloInstruction*, ReplacedAsync> replaced_pairs;
    bool should_update_schedule =
        module->has_schedule() &&
        module->schedule().is_computation_scheduled(computation);
    for (HloInstruction* instruction : supported_collectives) {
      if (HloAllReduceInstruction* ar =
              DynCast<HloAllReduceInstruction>(instruction)) {
        HloInstruction* start =
            computation->AddInstruction(HloInstruction::CreateAllReduceStart(
                ar->shape(), ar->operands(), ar->to_apply(),
                ar->replica_groups(), ar->constrain_layout(), ar->channel_id(),
                ar->use_global_device_ids()));
        std::unique_ptr<HloInstruction> done = HloInstruction::CreateUnary(
            ar->shape(), HloOpcode::kAllReduceDone, start);
        start->set_metadata(ar->metadata());
        start->CopyBackendConfigFrom(ar);
        if (should_update_schedule) {
          replaced_pairs[ar] = ReplacedAsync{start, done.get()};
        }
        TF_RETURN_WITH_CONTEXT_IF_ERROR(
            computation->ReplaceWithNewInstruction(ar, std::move(done)),
            "replacing ", ar->ToShortString());
        changed = true;
        continue;
      }
      if (HloAllGatherInstruction* ag =
              DynCast<HloAllGatherInstruction>(instruction)) {
        std::vector<const Shape*> operand_shapes;
        operand_shapes.reserve(ag->operand_count());
        for (const HloInstruction* op : ag->operands()) {
          operand_shapes.push_back(&op->shape());
        }
        Shape shape = ShapeUtil::MakeTupleShape(
            {ag->operand_count() > 1
                 ? ShapeUtil::MakeTupleShapeWithPtrs(operand_shapes)
                 : *operand_shapes[0],
             ag->shape()});
        HloInstruction* start =
            computation->AddInstruction(HloInstruction::CreateAllGatherStart(
                shape, ag->operands(), ag->all_gather_dimension(),
                ag->replica_groups(), ag->constrain_layout(), ag->channel_id(),
                ag->use_global_device_ids()));
        std::unique_ptr<HloInstruction> done = HloInstruction::CreateUnary(
            ag->shape(), HloOpcode::kAllGatherDone, start);
        start->set_metadata(ag->metadata());
        start->CopyBackendConfigFrom(ag);
        if (should_update_schedule) {
          replaced_pairs[ag] = ReplacedAsync{start, done.get()};
        }
        TF_RETURN_WITH_CONTEXT_IF_ERROR(
            computation->ReplaceWithNewInstruction(ag, std::move(done)),
            "replacing ", ag->ToShortString());
        changed = true;
        continue;
      }
      if (HloCollectivePermuteInstruction* cp =
              DynCast<HloCollectivePermuteInstruction>(instruction)) {
        HloInstruction* collective_permute_start;
        HloInstruction* operand = cp->mutable_operand(0);
        if (cp->operand_count() == 1) {
          collective_permute_start = computation->AddInstruction(
              HloInstruction::CreateCollectivePermuteStart(
                  ShapeUtil::MakeTupleShape(
                      {operand->shape(), cp->shape(),
                       ShapeUtil::MakeShape(U32, {}, {}),
                       ShapeUtil::MakeShape(U32, {}, {})}),
                  operand, cp->source_target_pairs(), cp->channel_id()));
        } else {
          CHECK_EQ(cp->operand_count(), 4);
          std::vector<const Shape*> operand_shapes;
          absl::c_transform(cp->operands(), std::back_inserter(operand_shapes),
                            [](const HloInstruction* operand) {
                              return &(operand->shape());
                            });
          collective_permute_start = computation->AddInstruction(
              HloInstruction::CreateCollectivePermuteStart(
                  ShapeInference::InferCollectivePermuteStartShape(
                      operand_shapes)
                      .value(),
                  operand, cp->mutable_operand(1), cp->mutable_operand(2),
                  cp->mutable_operand(3), cp->source_target_pairs(),
                  cp->dynamic_slice_sizes_list(), cp->channel_id()));
          if (HasDisjointReadWriteRegionsAttr(cp)) {
            SetDisjointReadWriteRegionsAttr(collective_permute_start);
          }
        }
        collective_permute_start->set_metadata(cp->metadata());
        collective_permute_start->CopyBackendConfigFrom(cp);
        HloInstruction* collective_permute_done = nullptr;
        if (!track_send_recv_separately_(cp)) {
          collective_permute_done =
              computation->AddInstruction(HloInstruction::CreateUnary(
                  cp->shape(), HloOpcode::kCollectivePermuteDone,
                  collective_permute_start));
        } else {
          /* track start done and end done separately using custom calls */
          HloInstruction* cp_recv_done =
              computation->AddInstruction(HloInstruction::CreateCustomCall(
                  cp->shape(), {collective_permute_start}, "$cp_recv_done"));

          HloInstruction* cp_send_done =
              computation->AddInstruction(HloInstruction::CreateCustomCall(
                  ShapeUtil::MakeTokenShape(), {collective_permute_start},
                  "$cp_send_done"));

          // Force cp_send_done to execute after cp_recv_done.
          TF_RETURN_IF_ERROR(
              cp_recv_done->AddControlDependencyTo(cp_send_done));

          // Mark these custom calls as having side effects they are not dead
          // code eliminated.
          Cast<HloCustomCallInstruction>(cp_send_done)
              ->set_custom_call_has_side_effect(true);
          Cast<HloCustomCallInstruction>(cp_recv_done)
              ->set_custom_call_has_side_effect(true);
          collective_permute_done = cp_recv_done;
        }
        if (should_update_schedule) {
          replaced_pairs[cp] =
              ReplacedAsync{collective_permute_start, collective_permute_done};
        }
        TF_RETURN_IF_ERROR(
            computation->ReplaceInstruction(cp, collective_permute_done));
        changed = true;
        continue;
      }
      if (HloAllToAllInstruction* ata =
              DynCast<HloAllToAllInstruction>(instruction)) {
        Shape sync_shape = ShapeUtil::MakeScalarShape(U32);
        TF_ASSIGN_OR_RETURN(HloInstruction * async_done,
                            computation->CreateAsyncInstructions(
                                ata, {sync_shape, sync_shape}));
        if (should_update_schedule) {
          HloInstruction* async_start = async_done->mutable_operand(0);
          replaced_pairs[ata] = ReplacedAsync{async_start, async_done};
        }
        changed = true;
        continue;
      }
    }
    if (should_update_schedule) {
      std::vector<HloInstruction*> new_sequence;
      const HloInstructionSequence& sequence =
          module->schedule().sequence(computation);
      new_sequence.reserve(sequence.size() + replaced_pairs.size());
      for (HloInstruction* instr : sequence.instructions()) {
        auto it = replaced_pairs.find(instr);
        if (it != replaced_pairs.end()) {
          new_sequence.push_back(it->second.start);
          new_sequence.push_back(it->second.done);
          continue;
        }
        new_sequence.push_back(instr);
      }
      module->schedule().set_sequence(computation, new_sequence);
    }
  }
  return changed;
}

}  // namespace xla
