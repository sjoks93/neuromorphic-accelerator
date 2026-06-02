#include "nmc/internal/core.h"

bool nmc_core_pair_weights_fit(const NmcCore *core,
                               nmc_tile_width_t input_width,
                               const NmcInputOutputPairLutEntry *pair_entry)
{
    if (!nmc_core_valid_width(input_width) || !nmc_core_valid_output_index(core, pair_entry->output_index)) {
        return false;
    }

    const size_t output_width = core->output_groups[pair_entry->output_index].route_lut.bitmap_width;
    const size_t pair_weights = (size_t)input_width * output_width;
    return pair_entry->weight_offset <= NMC_WEIGHT_MEMORY_SIZE && pair_weights <= NMC_WEIGHT_MEMORY_SIZE - pair_entry->weight_offset;
}

/*
 * Event-driven compute schedule: one input-lane adder-tree reduction per cycle.
 *
 * For one input/output pair, the core holds a small block of output-neuron
 * accumulators, walks the per-lane event streams in lockstep, and updates all
 * output lanes in the block in one cycle.  Zero input columns do not consume
 * compute cycles.
 */
bool nmc_core_accumulate_pair(NmcCore *core,
                              nmc_tile_width_t input_width,
                              const NmcInputOutputPairLutEntry *pair_entry,
                              NmcOutputGroup *output_group,
                              const NmcInputLaneEvents *lane_events)
{
    size_t accumulator_start = 0u;
    if (!nmc_core_output_accumulators_fit(core, pair_entry->output_index, &accumulator_start)) {
        return false;
    }

    uint32_t input_steps = 0u;
    for (size_t input_lane = 0u; input_lane < NMC_INPUT_PARALLELISM; ++input_lane) {
        if (input_steps < lane_events[input_lane].event_count) {
            input_steps = lane_events[input_lane].event_count;
        }
    }

    for (size_t out_base = 0; out_base < output_group->route_lut.bitmap_width; out_base += NMC_OUTPUT_PARALLELISM) {
        const size_t remaining_outputs = output_group->route_lut.bitmap_width - out_base;
        const size_t active_lanes = remaining_outputs < NMC_OUTPUT_PARALLELISM ? remaining_outputs : NMC_OUTPUT_PARALLELISM;
        for (uint32_t input_step = 0u; input_step < input_steps; ++input_step) {
            for (size_t lane = 0u; lane < active_lanes; ++lane) {
                const size_t out = out_base + lane;
                const size_t row_base = pair_entry->weight_offset + out * input_width;
                int32_t reduced_sum = 0;
                for (size_t input_lane = 0u; input_lane < NMC_INPUT_PARALLELISM; ++input_lane) {
                    if (input_step < lane_events[input_lane].event_count) {
                        const size_t weight_index = row_base + lane_events[input_lane].events[input_step];
                        reduced_sum += core->weights[weight_index];
                    }
                }
                core->accumulators[accumulator_start + out] += reduced_sum;
            }
            ++core->last_input_tile_compute_cycles;
            ++core->total_compute_cycles;
        }
    }

    return true;
}
