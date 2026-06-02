#include "nmc/internal/core.h"

/*
 * Convert one reshaped input lane into an ordered stream of non-zero event indices.
 *
 * The hardware priority search window is only P bits wide and produces at most
 * one event per encoder cycle.  If the current P-bit window is empty, the
 * encoder replaces it with the next P-bit window in the same cycle, giving
 * useful sparse skip behavior without widening the priority encoder.
 */
static bool encode_input_lane_events(const uint8_t *input_payload,
                                     nmc_tile_width_t input_width,
                                     nmc_tile_width_t lane_start,
                                     nmc_tile_width_t lane_width,
                                     NmcInputLaneEvents *lane_events)
{
    if (!nmc_core_valid_width(input_width) || lane_width == 0u || lane_start > input_width || lane_width > input_width - lane_start) {
        return false;
    }

    lane_events->event_count = 0u;
    lane_events->encoder_cycles = 0u;
    nmc_tile_width_t cursor = 0u;
    while (cursor < lane_width) {
        const nmc_tile_width_t window_start = (nmc_tile_width_t)((cursor / NMC_EVENT_ENCODER_WINDOW) * NMC_EVENT_ENCODER_WINDOW);
        const nmc_tile_width_t window_end = (nmc_tile_width_t)((window_start + NMC_EVENT_ENCODER_WINDOW) < lane_width ?
                                                                  (window_start + NMC_EVENT_ENCODER_WINDOW) :
                                                                  lane_width);

        ++lane_events->encoder_cycles;
        bool found_event = false;
        for (nmc_tile_width_t bit = cursor; bit < window_end; ++bit) {
            const nmc_tile_width_t global_bit = (nmc_tile_width_t)(lane_start + bit);
            if (nmc_core_payload_bit_is_set(input_payload, global_bit)) {
                lane_events->events[lane_events->event_count++] = global_bit;
                cursor = (nmc_tile_width_t)(bit + 1u);
                found_event = true;
                break;
            }
        }
        if (found_event) {
            continue;
        }

        const nmc_tile_width_t replacement_start = window_end;
        if (replacement_start >= lane_width) {
            break;
        }

        const nmc_tile_width_t replacement_end = (nmc_tile_width_t)((replacement_start + NMC_EVENT_ENCODER_WINDOW) < lane_width ?
                                                                        (replacement_start + NMC_EVENT_ENCODER_WINDOW) :
                                                                        lane_width);
        cursor = replacement_end;
        for (nmc_tile_width_t bit = replacement_start; bit < replacement_end; ++bit) {
            const nmc_tile_width_t global_bit = (nmc_tile_width_t)(lane_start + bit);
            if (nmc_core_payload_bit_is_set(input_payload, global_bit)) {
                lane_events->events[lane_events->event_count++] = global_bit;
                cursor = (nmc_tile_width_t)(bit + 1u);
                break;
            }
        }
    }

    return true;
}

bool nmc_core_encode_input_events(const uint8_t *input_payload,
                                  nmc_tile_width_t input_width,
                                  NmcInputLaneEvents *lane_events,
                                  uint32_t *event_count,
                                  uint64_t *encoder_cycles)
{
    if (!nmc_core_valid_width(input_width) || input_width < NMC_INPUT_PARALLELISM ||
        (input_width % NMC_INPUT_PARALLELISM) != 0u) {
        return false;
    }

    *event_count = 0u;
    *encoder_cycles = 0u;
    const nmc_tile_width_t lane_width = (nmc_tile_width_t)(input_width / NMC_INPUT_PARALLELISM);
    for (nmc_tile_width_t lane = 0u; lane < NMC_INPUT_PARALLELISM; ++lane) {
        const nmc_tile_width_t lane_start = (nmc_tile_width_t)(lane * lane_width);
        if (!encode_input_lane_events(input_payload, input_width, lane_start, lane_width, &lane_events[lane])) {
            return false;
        }
        *event_count += lane_events[lane].event_count;
        if (*encoder_cycles < lane_events[lane].encoder_cycles) {
            *encoder_cycles = lane_events[lane].encoder_cycles;
        }
    }

    return true;
}
