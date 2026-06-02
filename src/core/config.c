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

    NmcOutputGroup *group = &core->output_groups[core->output_group_count++];
    memset(group, 0, sizeof(*group));
    group->route_lut.bitmap_width = width;
    for (nmc_tile_width_t i = 0; i < width; ++i) {
        group->neurons[i].threshold = thresholds ? thresholds[i] : 1;
    }
    return true;
}

bool nmc_core_set_output_accumulator_lut_start(NmcCore *core,
                                               nmc_output_index_t output_index,
                                               size_t accumulator_start)
{
    if (!nmc_core_valid_output_index(core, output_index) || accumulator_start >= NMC_UNIFIED_MEMORY_SIZE) {
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
