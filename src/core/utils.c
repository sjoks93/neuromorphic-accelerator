#include "nmc/internal/core.h"

#include <limits.h>
#include <stdio.h>

_Static_assert(CHAR_BIT == 8, "NMC memory packing requires 8-bit bytes");
_Static_assert(sizeof(int16_t) * CHAR_BIT == NMC_WEIGHT_LANE_BITS, "NMC weight lane width must match int16_t");
_Static_assert(sizeof(int32_t) * CHAR_BIT == NMC_ACCUMULATOR_BITS, "NMC accumulator width must match int32_t");
_Static_assert(NMC_INPUT_PARALLELISM >= NMC_ACCUMULATOR_LANES, "input lanes must hold at least one accumulator value");

bool nmc_core_valid_input_index(const NmcCore *core, nmc_input_index_t input_index)
{
    return input_index < core->input_group_count;
}

bool nmc_core_valid_ack_index(const NmcCore *core, nmc_ack_index_t ack_index)
{
    return ack_index < core->ack_group_count;
}

bool nmc_core_valid_output_index(const NmcCore *core, nmc_output_index_t output_index)
{
    return output_index < core->output_group_count;
}

bool nmc_core_valid_width(nmc_tile_width_t width)
{
    return width > 0u && width <= NMC_MAX_GROUP_NEURONS && (width % NMC_BITS_PER_BYTE) == 0u;
}

size_t nmc_core_payload_bytes(nmc_tile_width_t width)
{
    return (size_t)width / NMC_BITS_PER_BYTE;
}

bool nmc_core_payload_bit_is_set(const uint8_t *payload, size_t bit_index)
{
    return (payload[bit_index / NMC_BITS_PER_BYTE] & (uint8_t)(UINT8_C(1) << (bit_index % NMC_BITS_PER_BYTE))) != 0u;
}

void nmc_core_payload_set_bit(uint8_t *payload, size_t bit_index)
{
    payload[bit_index / NMC_BITS_PER_BYTE] |= (uint8_t)(UINT8_C(1) << (bit_index % NMC_BITS_PER_BYTE));
}

bool nmc_core_output_accumulators_fit(const NmcCore *core,
                                      nmc_output_index_t output_index,
                                      size_t *accumulator_start)
{
    if (!nmc_core_valid_output_index(core, output_index)) {
        return false;
    }

    const NmcIndirectAddress *entry = &core->accumulator_lut[output_index];
    const size_t output_width = core->output_groups[output_index].route_lut.bitmap_width;
    const size_t lane_span = nmc_core_accumulator_lane_span((nmc_tile_width_t)output_width);
    if (!entry->valid || entry->start > NMC_UNIFIED_MEMORY_SIZE) {
        return false;
    }
    if (lane_span > NMC_UNIFIED_MEMORY_SIZE - entry->start) {
        return false;
    }

    *accumulator_start = entry->start;
    return true;
}

size_t nmc_core_accumulator_lane_span(nmc_tile_width_t output_width)
{
    const size_t accumulators_per_row = NMC_INPUT_PARALLELISM / NMC_ACCUMULATOR_LANES;
    const size_t row_count = ((size_t)output_width + accumulators_per_row - 1u) / accumulators_per_row;
    return row_count * NMC_INPUT_PARALLELISM;
}

size_t nmc_core_accumulator_lane_address(size_t accumulator_start, size_t output_index)
{
    const size_t accumulators_per_row = NMC_INPUT_PARALLELISM / NMC_ACCUMULATOR_LANES;
    const size_t row = output_index / accumulators_per_row;
    const size_t row_accumulator = output_index % accumulators_per_row;
    return accumulator_start + row * NMC_INPUT_PARALLELISM + row_accumulator * NMC_ACCUMULATOR_LANES;
}

size_t nmc_core_accumulator_mux_index(size_t accumulator_lane_address)
{
    return accumulator_lane_address % NMC_INPUT_PARALLELISM;
}

bool nmc_core_memory_read_weight_lane(const NmcCore *core,
                                      size_t lane_address,
                                      NmcMemoryOutputRoute route,
                                      int16_t *value)
{
    if (route != NMC_MEMORY_OUTPUT_TO_ADDER_TREE || lane_address >= NMC_UNIFIED_MEMORY_SIZE || value == NULL) {
        return false;
    }

    *value = core->memory[lane_address];
    return true;
}

bool nmc_core_memory_read_accumulator(const NmcCore *core,
                                      size_t accumulator_start,
                                      size_t output_index,
                                      NmcMemoryOutputRoute route,
                                      int32_t *value)
{
    if (route != NMC_MEMORY_OUTPUT_TO_ACCUMULATOR_MUX || value == NULL || NMC_ACCUMULATOR_LANES > NMC_INPUT_PARALLELISM) {
        return false;
    }

    const size_t lane_address = nmc_core_accumulator_lane_address(accumulator_start, output_index);
    const size_t mux_index = nmc_core_accumulator_mux_index(lane_address);
    if (mux_index > NMC_INPUT_PARALLELISM || NMC_ACCUMULATOR_LANES > NMC_INPUT_PARALLELISM - mux_index) {
        return false;
    }
    if (lane_address > NMC_UNIFIED_MEMORY_SIZE || NMC_ACCUMULATOR_LANES > NMC_UNIFIED_MEMORY_SIZE - lane_address) {
        return false;
    }

    uint32_t bits = 0u;
    for (size_t lane = 0u; lane < NMC_ACCUMULATOR_LANES; ++lane) {
        bits |= (uint32_t)(uint16_t)core->memory[lane_address + lane] << (lane * NMC_WEIGHT_LANE_BITS);
    }
    *value = (int32_t)bits;
    return true;
}

bool nmc_core_memory_write_accumulator(NmcCore *core,
                                       size_t accumulator_start,
                                       size_t output_index,
                                       int32_t value)
{
    if (NMC_ACCUMULATOR_LANES > NMC_INPUT_PARALLELISM) {
        return false;
    }

    const size_t lane_address = nmc_core_accumulator_lane_address(accumulator_start, output_index);
    const size_t mux_index = nmc_core_accumulator_mux_index(lane_address);
    if (mux_index > NMC_INPUT_PARALLELISM || NMC_ACCUMULATOR_LANES > NMC_INPUT_PARALLELISM - mux_index) {
        return false;
    }
    if (lane_address > NMC_UNIFIED_MEMORY_SIZE || NMC_ACCUMULATOR_LANES > NMC_UNIFIED_MEMORY_SIZE - lane_address) {
        return false;
    }

    const uint32_t bits = (uint32_t)value;
    for (size_t lane = 0u; lane < NMC_ACCUMULATOR_LANES; ++lane) {
        core->memory[lane_address + lane] = (int16_t)((bits >> (lane * NMC_WEIGHT_LANE_BITS)) & UINT16_MAX);
    }
    return true;
}

void nmc_print_payload(const uint8_t *payload, nmc_tile_width_t width)
{
    putchar('0');
    putchar('b');
    for (int i = (int)width - 1; i >= 0; --i) {
        putchar(nmc_core_payload_bit_is_set(payload, (size_t)i) ? '1' : '0');
    }
}
