#include "nmc/internal/core.h"

#include <string.h>

bool nmc_core_add_input_group(NmcCore *core)
{
    if (core->input_group_count >= NMC_MAX_INPUT_GROUPS) {
        return false;
    }

    core->input_groups[core->input_group_count++] = (NmcInputGroup){0};
    return true;
}

bool nmc_core_add_ack_group(NmcCore *core)
{
    if (core->ack_group_count >= NMC_MAX_ACK_GROUPS) {
        return false;
    }

    core->ack_groups[core->ack_group_count++] = (NmcAckGroup){0};
    return true;
}

bool nmc_core_add_output_group(NmcCore *core, nmc_tile_width_t width, const int32_t *thresholds)
{
    if (!nmc_core_valid_width(width) || core->output_group_count >= NMC_MAX_OUTPUT_GROUPS) {
        return false;
    }

    const int32_t threshold = thresholds ? thresholds[0] : 1;
    for (nmc_tile_width_t i = 1u; thresholds != NULL && i < width; ++i) {
        if (thresholds[i] != threshold) {
            return false;
        }
    }

    NmcOutputGroup *group = &core->output_groups[core->output_group_count++];
    memset(group, 0, sizeof(*group));
    group->route_lut.bitmap_width = width;
    group->activation_program_index = NMC_ACTIVATION_PROGRAM_IF_IMMEDIATE;
    group->activation_immediates[NMC_ACTIVATION_THRESHOLD_IMMEDIATE] = threshold;
    return true;
}

bool nmc_core_add_activation_program(NmcCore *core,
                                     const NmcActivationInstruction *instructions,
                                     size_t instruction_count,
                                     uint32_t max_steps,
                                     uint32_t *program_index)
{
    if (core == NULL || instructions == NULL || instruction_count == 0u ||
        core->activation_program_count >= NMC_MAX_ACTIVATION_PROGRAMS ||
        instruction_count > NMC_MAX_ACTIVATION_INSTRUCTIONS - core->activation_instruction_count ||
        max_steps == 0u) {
        return false;
    }

    bool has_end = false;
    for (size_t i = 0u; i < instruction_count; ++i) {
        if (instructions[i].opcode == NMC_ACT_OP_END) {
            has_end = true;
            break;
        }
    }
    if (!has_end) {
        return false;
    }

    const uint32_t index = (uint32_t)core->activation_program_count;
    const size_t start = core->activation_instruction_count;
    memcpy(&core->activation_instructions[start], instructions, instruction_count * sizeof(instructions[0]));
    core->activation_programs[core->activation_program_count++] = (NmcActivationProgramDescriptor){
        .valid = true,
        .start = start,
        .length = instruction_count,
        .max_steps = max_steps,
    };
    core->activation_instruction_count += instruction_count;

    if (program_index != NULL) {
        *program_index = index;
    }
    return true;
}

bool nmc_core_bind_output_activation_program(NmcCore *core,
                                             nmc_output_index_t output_index,
                                             uint32_t program_index)
{
    if (!nmc_core_valid_output_index(core, output_index) ||
        program_index >= core->activation_program_count ||
        !core->activation_programs[program_index].valid) {
        return false;
    }

    core->output_groups[output_index].activation_program_index = program_index;
    return true;
}

bool nmc_core_bind_activation_sram_range(NmcCore *core,
                                         nmc_output_index_t output_index,
                                         uint8_t range_index,
                                         size_t start,
                                         size_t count)
{
    if (count > SIZE_MAX / NMC_ACTIVATION_WORD_LANES) {
        return false;
    }

    const size_t lane_span = count * NMC_ACTIVATION_WORD_LANES;
    if (!nmc_core_valid_output_index(core, output_index) ||
        range_index >= NMC_MAX_ACTIVATION_SRAM_RANGES ||
        count == 0u ||
        start > NMC_UNIFIED_MEMORY_SIZE ||
        lane_span > NMC_UNIFIED_MEMORY_SIZE - start) {
        return false;
    }

    core->output_groups[output_index].activation_sram_ranges[range_index] = (NmcActivationSramRange){
        .valid = true,
        .start = start,
        .count = count,
    };
    return true;
}

bool nmc_core_set_activation_immediate(NmcCore *core,
                                       nmc_output_index_t output_index,
                                       uint8_t immediate_index,
                                       int32_t value)
{
    if (!nmc_core_valid_output_index(core, output_index) || immediate_index >= NMC_MAX_ACTIVATION_IMMEDIATES) {
        return false;
    }

    core->output_groups[output_index].activation_immediates[immediate_index] = value;
    return true;
}

bool nmc_core_set_output_accumulator_lut_start(NmcCore *core,
                                               nmc_output_index_t output_index,
                                               size_t accumulator_start)
{
    if (!nmc_core_valid_output_index(core, output_index) ||
        accumulator_start >= NMC_UNIFIED_MEMORY_SIZE ||
        (accumulator_start % NMC_ACTIVATION_SRAM_LANES) != 0u) {
        return false;
    }

    const size_t output_width = core->output_groups[output_index].route_lut.bitmap_width;
    const size_t lane_span = nmc_core_accumulator_lane_span((nmc_tile_width_t)output_width);
    if (lane_span > NMC_UNIFIED_MEMORY_SIZE - accumulator_start) {
        return false;
    }

    core->accumulator_lut[output_index] = (NmcIndirectAddress){
        .valid = true,
        .start = accumulator_start,
    };
    return true;
}

bool nmc_core_set_output_lut_starts(NmcCore *core,
                                    nmc_output_index_t output_index,
                                    size_t successor_start,
                                    size_t predecessor_start)
{
    /*
     * output_index == output_group_count configures the terminal sentinel.
     * The sentinel has an empty successor/predecessor range and terminates the
     * predecessor range of the last real output group.
     */
    if (output_index > core->output_group_count ||
        successor_start > predecessor_start ||
        predecessor_start > core->output_route_lut_count) {
        return false;
    }
    if (output_index == core->output_group_count && successor_start != predecessor_start) {
        return false;
    }

    NmcOutputGroup *group = &core->output_groups[output_index];
    group->route_lut.valid = true;
    group->route_lut.successor_start = successor_start;
    group->route_lut.predecessor_start = predecessor_start;
    if (output_index < core->output_group_count) {
        /* No prior tile is outstanding after configuration, so the group starts synchronized. */
        group->ack_count = (uint32_t)(predecessor_start - successor_start);
    }
    return true;
}

static bool add_output_route_lut_entry(NmcCore *core,
                                       nmc_core_id_t core_id,
                                       nmc_input_index_t group_index,
                                       bool recurrent)
{
    if (core->output_route_lut_count >= NMC_MAX_OUTPUT_ROUTE_LUT_ENTRIES) {
        return false;
    }

    core->output_route_lut[core->output_route_lut_count++] = (NmcOutputRouteLutEntry){
        .address = {
            .core_id = core_id,
            .group_index = group_index,
        },
        .recurrent = recurrent,
    };
    return true;
}

bool nmc_core_add_output_successor_lut_entry(NmcCore *core,
                                             nmc_core_id_t target_core_id,
                                             nmc_input_index_t target_group_index)
{
    return add_output_route_lut_entry(core, target_core_id, target_group_index, false);
}

bool nmc_core_add_output_predecessor_lut_entry(NmcCore *core,
                                               nmc_core_id_t predecessor_core_id,
                                               nmc_input_index_t predecessor_group_index)
{
    return add_output_route_lut_entry(core, predecessor_core_id, predecessor_group_index, false);
}

bool nmc_core_add_recurrent_output_predecessor_lut_entry(NmcCore *core,
                                                         nmc_core_id_t predecessor_core_id,
                                                         nmc_input_index_t predecessor_group_index)
{
    return add_output_route_lut_entry(core, predecessor_core_id, predecessor_group_index, true);
}

bool nmc_core_set_input_lut_start(NmcCore *core,
                                  nmc_input_index_t input_index,
                                  size_t pair_start)
{
    /* input_index == input_group_count configures the terminal pair-LUT start. */
    if (input_index > core->input_group_count || pair_start > core->input_output_pair_lut_count) {
        return false;
    }

    NmcInputGroup *input_group = &core->input_groups[input_index];
    input_group->lut.valid = true;
    input_group->lut.start = pair_start;
    return true;
}

bool nmc_core_set_ack_lut_start(NmcCore *core,
                                nmc_ack_index_t ack_index,
                                size_t pair_start)
{
    /* ack_index == ack_group_count configures the terminal ACK-LUT start. */
    if (ack_index > core->ack_group_count || pair_start > core->ack_output_pair_lut_count) {
        return false;
    }

    NmcAckGroup *ack_group = &core->ack_groups[ack_index];
    ack_group->lut.valid = true;
    ack_group->lut.start = pair_start;
    return true;
}

static bool add_input_output_pair_lut_entry(NmcCore *core,
                                            nmc_output_index_t output_index,
                                            size_t weight_offset,
                                            bool recurrent)
{
    if (core->input_output_pair_lut_count >= NMC_MAX_INPUT_OUTPUT_PAIR_LUT_ENTRIES) {
        return false;
    }

    if (!nmc_core_valid_output_index(core, output_index) || weight_offset >= NMC_UNIFIED_MEMORY_SIZE) {
        return false;
    }

    core->input_output_pair_lut[core->input_output_pair_lut_count++] = (NmcInputOutputPairLutEntry){
        .output_index = output_index,
        .weight_offset = weight_offset,
        .recurrent = recurrent,
    };

    NmcOutputGroup *output_group = &core->output_groups[output_index];
    ++output_group->input_requirement;
    if (recurrent) {
        ++output_group->input_count;
        ++output_group->primed_recurrent_input_count;
    }
    return true;
}

bool nmc_core_add_input_output_pair_lut_entry(NmcCore *core,
                                              nmc_output_index_t output_index,
                                              size_t weight_offset)
{
    return add_input_output_pair_lut_entry(core, output_index, weight_offset, false);
}

bool nmc_core_add_recurrent_input_output_pair_lut_entry(NmcCore *core,
                                                        nmc_output_index_t output_index,
                                                        size_t weight_offset)
{
    return add_input_output_pair_lut_entry(core, output_index, weight_offset, true);
}

bool nmc_core_add_ack_output_pair_lut_entry(NmcCore *core,
                                            nmc_output_index_t output_index)
{
    if (core->ack_output_pair_lut_count >= NMC_MAX_ACK_OUTPUT_PAIR_LUT_ENTRIES) {
        return false;
    }

    if (!nmc_core_valid_output_index(core, output_index)) {
        return false;
    }

    core->ack_output_pair_lut[core->ack_output_pair_lut_count++] = (NmcAckOutputPairLutEntry){
        .output_index = output_index,
    };
    return true;
}
