#include "nmc/internal/core.h"

bool nmc_core_process_input_tile(NmcCore *core, const NmcInputTile *tile)
{
    if (!nmc_core_valid_input_index(core, tile->group_index) || !nmc_core_valid_width(tile->width)) {
        return false;
    }

    const NmcInputGroup *input_group = &core->input_groups[tile->group_index];
    if (input_group->width != 0u && input_group->width != tile->width) {
        return false;
    }

    core->last_input_tile_compute_cycles = 0u;
    core->last_input_tile_encoder_cycles = 0u;
    core->last_input_tile_event_count = 0u;

    NmcInputLaneEvents lane_events[NMC_MAX_GROUP_NEURONS];
    uint32_t event_count = 0u;
    uint64_t encoder_cycles = 0u;
    if (!nmc_core_encode_input_events(tile->payload, tile->width, lane_events, &event_count, &encoder_cycles)) {
        return false;
    }
    core->last_input_tile_event_count = event_count;
    core->last_input_tile_encoder_cycles = encoder_cycles;
    core->total_encoder_cycles += encoder_cycles;

    /* The following input-group entry provides the exclusive end of this range. */
    const NmcInputGroup *next_input_group = &core->input_groups[tile->group_index + 1u];
    if (!input_group->lut.valid || !next_input_group->lut.valid || input_group->lut.start > next_input_group->lut.start) {
        return false;
    }

    /* Walk all output groups subscribed to this input group. */
    bool matched = false;
    for (size_t i = input_group->lut.start; i < next_input_group->lut.start; ++i) {
        NmcInputOutputPairLutEntry *pair_entry = &core->input_output_pair_lut[i];
        if (!nmc_core_valid_output_index(core, pair_entry->output_index)) {
            return false;
        }
        if (!nmc_core_pair_weights_fit(core, tile->width, pair_entry)) {
            return false;
        }

        NmcOutputGroup *output_group = &core->output_groups[pair_entry->output_index];
        if (output_group->input_count >= output_group->input_requirement) {
            return false;
        }

        /* One arrival contributes once to this output group's current step. */
        if (!nmc_core_accumulate_pair(core, tile->width, pair_entry, output_group, lane_events)) {
            return false;
        }
        ++output_group->input_count;
        matched = true;

        if (nmc_core_output_group_ready(output_group)) {
            (void)nmc_core_activate_output_group(core, pair_entry->output_index);
        }
    }

    return matched;
}
