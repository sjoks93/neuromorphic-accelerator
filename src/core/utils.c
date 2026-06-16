#include "nmc/internal/core.h"

#include <limits.h>
#include <stdio.h>

_Static_assert(CHAR_BIT == 8, "NMC memory packing requires 8-bit bytes");
_Static_assert(NMC_WEIGHT_LANE_BITS > 0u && NMC_WEIGHT_LANE_BITS <= (sizeof(int16_t) * CHAR_BIT),
               "NMC weight lane width must fit inside int16_t storage lanes");
_Static_assert(NMC_ACCUMULATOR_BITS > 0u && NMC_ACCUMULATOR_BITS <= 32u,
               "NMC accumulator width must fit within int32_t accumulator API");
_Static_assert(NMC_ACTIVATION_WORD_BITS > 0u && NMC_ACTIVATION_WORD_BITS <= 32u,
               "NMC activation word width must fit within int32_t activation API");
_Static_assert(NMC_INPUT_PARALLELISM >= NMC_ACCUMULATOR_LANES, "input lanes must hold at least one accumulator value");
_Static_assert((NMC_ACTIVATION_SRAM_LANES % NMC_ACTIVATION_MEMBRANE_WORDS) == 0u,
               "membrane word count must divide the activation SRAM lane count");

static uint32_t lane_mask(void)
{
    if (NMC_WEIGHT_LANE_BITS == 32u) {
        return UINT32_MAX;
    }
    return (UINT32_C(1) << NMC_WEIGHT_LANE_BITS) - UINT32_C(1);
}

static int32_t sign_extend_to_i32(uint32_t value, uint32_t bits)
{
    if (bits == 0u || bits >= 32u) {
        return (int32_t)value;
    }
    const uint32_t sign_bit = UINT32_C(1) << (bits - 1u);
    const uint32_t full_mask = (UINT32_C(1) << bits) - UINT32_C(1);
    value &= full_mask;
    if ((value & sign_bit) != 0u) {
        value |= ~full_mask;
    }
    return (int32_t)value;
}

static bool signed_fits_bits(int32_t value, uint32_t bits)
{
    if (bits == 0u || bits >= 32u) {
        return true;
    }
    const int32_t min_value = -(int32_t)(UINT32_C(1) << (bits - 1u));
    const int32_t max_value = (int32_t)((UINT32_C(1) << (bits - 1u)) - UINT32_C(1));
    return value >= min_value && value <= max_value;
}

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

    *value = (int16_t)sign_extend_to_i32((uint32_t)(uint16_t)core->memory[lane_address], NMC_WEIGHT_LANE_BITS);
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
    const uint32_t mask = lane_mask();
    for (size_t lane = 0u; lane < NMC_ACCUMULATOR_LANES; ++lane) {
        const uint32_t lane_bits = ((uint32_t)(uint16_t)core->memory[lane_address + lane]) & mask;
        bits |= lane_bits << (lane * NMC_WEIGHT_LANE_BITS);
    }
    *value = sign_extend_to_i32(bits, NMC_ACCUMULATOR_BITS);
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
    const uint32_t mask = lane_mask();
    for (size_t lane = 0u; lane < NMC_ACCUMULATOR_LANES; ++lane) {
        core->memory[lane_address + lane] = (int16_t)((bits >> (lane * NMC_WEIGHT_LANE_BITS)) & mask);
    }
    return true;
}

bool nmc_core_memory_read_activation_word(const NmcCore *core,
                                          size_t lane_address,
                                          int32_t *value)
{
    if (core == NULL || value == NULL || lane_address > SIZE_MAX / NMC_ACTIVATION_WORD_LANES) {
        return false;
    }

    const size_t start = lane_address * NMC_ACTIVATION_WORD_LANES;
    if (start > NMC_UNIFIED_MEMORY_SIZE || NMC_ACTIVATION_WORD_LANES > NMC_UNIFIED_MEMORY_SIZE - start) {
        return false;
    }

    uint32_t bits = 0u;
    const uint32_t mask = lane_mask();
    for (size_t lane = 0u; lane < NMC_ACTIVATION_WORD_LANES; ++lane) {
        const uint32_t lane_bits = ((uint32_t)(uint16_t)core->memory[start + lane]) & mask;
        bits |= lane_bits << (lane * NMC_WEIGHT_LANE_BITS);
    }

    *value = sign_extend_to_i32(bits, NMC_ACTIVATION_WORD_BITS);
    return true;
}

bool nmc_core_memory_write_activation_word(NmcCore *core,
                                           size_t lane_address,
                                           int32_t value)
{
    if (core == NULL || lane_address > SIZE_MAX / NMC_ACTIVATION_WORD_LANES ||
        !signed_fits_bits(value, NMC_ACTIVATION_WORD_BITS)) {
        return false;
    }

    const size_t start = lane_address * NMC_ACTIVATION_WORD_LANES;
    if (start > NMC_UNIFIED_MEMORY_SIZE || NMC_ACTIVATION_WORD_LANES > NMC_UNIFIED_MEMORY_SIZE - start) {
        return false;
    }

    const uint32_t bits = (uint32_t)value;
    const uint32_t mask = lane_mask();
    for (size_t lane = 0u; lane < NMC_ACTIVATION_WORD_LANES; ++lane) {
        core->memory[start + lane] = (int16_t)((bits >> (lane * NMC_WEIGHT_LANE_BITS)) & mask);
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
