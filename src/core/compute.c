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
    return pair_entry->weight_offset <= NMC_UNIFIED_MEMORY_SIZE && pair_weights <= NMC_UNIFIED_MEMORY_SIZE - pair_entry->weight_offset;
}

/*
 * Event-driven compute schedule: one input-lane adder-tree reduction per cycle.
 *
 * For one input/output pair, the weight SRAM is partitioned into input-lane
 * banks.  Each bank row is output-wide, so one non-zero input event reads the
 * full vector of M weights for that input and updates the M-wide accumulator
 * row.  Zero input columns do not consume compute cycles.
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

    const size_t output_width = output_group->route_lut.bitmap_width;
    for (uint32_t input_step = 0u; input_step < input_steps; ++input_step) {
        for (size_t out = 0u; out < output_width; ++out) {
            int32_t reduced_sum = 0;
            for (size_t input_lane = 0u; input_lane < NMC_INPUT_PARALLELISM; ++input_lane) {
                if (input_step < lane_events[input_lane].event_count) {
                    const size_t weight_bank = lane_events[input_lane].events[input_step];
                    int16_t weight = 0;
                    if (weight_bank >= input_width) {
                        return false;
                    }
                    const size_t weight_index = pair_entry->weight_offset + weight_bank * output_width + out;
                    if (!nmc_core_memory_read_weight_lane(core, weight_index, NMC_MEMORY_OUTPUT_TO_ADDER_TREE, &weight)) {
                        return false;
                    }
                    reduced_sum += weight;
                }
            }
            int32_t accumulator = 0;
            if (!nmc_core_memory_read_accumulator(core,
                                                  accumulator_start,
                                                  out,
                                                  NMC_MEMORY_OUTPUT_TO_ACCUMULATOR_MUX,
                                                  &accumulator) ||
                !nmc_core_memory_write_accumulator(core, accumulator_start, out, accumulator + reduced_sum)) {
                return false;
            }
        }
        ++core->last_input_tile_compute_cycles;
        ++core->total_compute_cycles;
    }

    return true;
}
