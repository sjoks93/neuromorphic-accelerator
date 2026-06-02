#include "nmc/internal/core.h"

#include <stdio.h>

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
    if (!entry->valid || entry->start > NMC_ACCUMULATOR_MEMORY_SIZE) {
        return false;
    }
    if (output_width > NMC_ACCUMULATOR_MEMORY_SIZE - entry->start) {
        return false;
    }

    *accumulator_start = entry->start;
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
