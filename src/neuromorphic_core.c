#include "neuromorphic_core.h"

#include <stdio.h>
#include <string.h>

static bool valid_input_index(const NmcCore *core, nmc_input_index_t input_index)
{
    return input_index < core->input_group_count;
}

static bool valid_output_index(const NmcCore *core, nmc_output_index_t output_index)
{
    return output_index < core->output_group_count;
}

static bool valid_width(nmc_tile_width_t width)
{
    return width > 0u && width <= NMC_MAX_GROUP_NEURONS && (width % NMC_BITS_PER_BYTE) == 0u;
}

static size_t payload_bytes(nmc_tile_width_t width)
{
    return (size_t)width / NMC_BITS_PER_BYTE;
}

static bool payload_bit_is_set(const uint8_t *payload, size_t bit_index)
{
    return (payload[bit_index / NMC_BITS_PER_BYTE] & (uint8_t)(UINT8_C(1) << (bit_index % NMC_BITS_PER_BYTE))) != 0u;
}

static void payload_set_bit(uint8_t *payload, size_t bit_index)
{
    payload[bit_index / NMC_BITS_PER_BYTE] |= (uint8_t)(UINT8_C(1) << (bit_index % NMC_BITS_PER_BYTE));
}

static bool pair_weights_fit(const NmcCore *core, nmc_input_index_t input_index, const NmcInputOutputPairLutEntry *pair_entry)
{
    if (!valid_input_index(core, input_index) || !valid_output_index(core, pair_entry->output_index)) {
        return false;
    }

    const size_t input_width = core->input_groups[input_index].width;
    const size_t output_width = core->output_groups[pair_entry->output_index].width;
    const size_t pair_weights = input_width * output_width;
    return pair_entry->weight_offset <= core->weight_count && pair_weights <= core->weight_count - pair_entry->weight_offset;
}

void nmc_core_init(NmcCore *core, nmc_core_id_t core_id, int16_t *weights, size_t weight_count)
{
    memset(core, 0, sizeof(*core));
    core->core_id = core_id;
    core->weights = weights;
    core->weight_count = weight_count;
}

bool nmc_core_add_input_group(NmcCore *core, nmc_tile_width_t width)
{
    if (!valid_width(width) || core->input_group_count >= NMC_MAX_INPUT_GROUPS) {
        return false;
    }

    core->input_groups[core->input_group_count++] = (NmcInputGroup){
        .width = width,
    };
    return true;
}

bool nmc_core_add_output_group(NmcCore *core, nmc_tile_width_t width, const int32_t *thresholds)
{
    if (!valid_width(width) || core->output_group_count >= NMC_MAX_OUTPUT_GROUPS) {
        return false;
    }

    NmcOutputGroup *group = &core->output_groups[core->output_group_count++];
    memset(group, 0, sizeof(*group));
    group->width = width;
    for (nmc_tile_width_t i = 0; i < width; ++i) {
        group->neurons[i].threshold = thresholds ? thresholds[i] : 1;
    }
    return true;
}

bool nmc_core_set_output_lut_range(NmcCore *core,
                                   nmc_output_index_t output_index,
                                   size_t destination_start,
                                   size_t destination_end)
{
    if (!valid_output_index(core, output_index) || destination_start > destination_end || destination_end > core->output_destination_lut_count) {
        return false;
    }

    NmcOutputGroup *group = &core->output_groups[output_index];
    group->lut.valid = true;
    group->lut.start = destination_start;
    group->lut.end = destination_end;
    return true;
}

bool nmc_core_add_output_destination_lut_entry(NmcCore *core,
                                               nmc_core_id_t target_core_id,
                                               nmc_input_index_t target_group_index)
{
    if (core->output_destination_lut_count >= NMC_MAX_OUTPUT_DESTINATION_LUT_ENTRIES) {
        return false;
    }

    core->output_destination_lut[core->output_destination_lut_count++] = (NmcDestinationAddress){
        .core_id = target_core_id,
        .group_index = target_group_index,
    };
    return true;
}

bool nmc_core_set_input_lut_range(NmcCore *core,
                                  nmc_input_index_t input_index,
                                  size_t pair_start,
                                  size_t pair_end)
{
    if (!valid_input_index(core, input_index) || pair_start > pair_end || pair_end > core->input_output_pair_lut_count) {
        return false;
    }

    NmcInputGroup *input_group = &core->input_groups[input_index];
    input_group->lut.valid = true;
    input_group->lut.start = pair_start;
    input_group->lut.end = pair_end;
    for (size_t i = pair_start; i < pair_end; ++i) {
        if (!pair_weights_fit(core, input_index, &core->input_output_pair_lut[i])) {
            input_group->lut = (NmcIndirectAddress){0};
            return false;
        }
    }

    return true;
}

bool nmc_core_add_input_output_pair_lut_entry(NmcCore *core,
                                              nmc_output_index_t output_index,
                                              size_t weight_offset)
{
    if (core->input_output_pair_lut_count >= NMC_MAX_INPUT_OUTPUT_PAIR_LUT_ENTRIES) {
        return false;
    }

    if (!valid_output_index(core, output_index) || weight_offset >= core->weight_count) {
        return false;
    }

    core->input_output_pair_lut[core->input_output_pair_lut_count++] = (NmcInputOutputPairLutEntry){
        .output_index = output_index,
        .weight_offset = weight_offset,
    };

    NmcOutputGroup *output_group = &core->output_groups[output_index];
    ++output_group->input_count;
    output_group->remaining_input_count = output_group->input_count;
    return true;
}

static void accumulate_pair(NmcCore *core,
                            const NmcInputGroup *input_group,
                            const NmcInputOutputPairLutEntry *pair_entry,
                            NmcOutputGroup *output_group,
                            const uint8_t *input_payload)
{
    for (size_t out = 0; out < output_group->width; ++out) {
        int32_t sum = 0;
        for (size_t in = 0; in < input_group->width; ++in) {
            if (payload_bit_is_set(input_payload, in)) {
                const size_t weight_index = pair_entry->weight_offset + out * input_group->width + in;
                sum += core->weights[weight_index];
            }
        }
        output_group->neurons[out].accumulator += sum;
    }
}

static bool output_group_ready(const NmcOutputGroup *group)
{
    return group->input_count != 0u && group->remaining_input_count == 0u;
}

static bool enqueue_network_tile(NmcCore *core, const NmcOutputGroup *group, const uint8_t *payload)
{
    if (!group->lut.valid) {
        return false;
    }

    const size_t destination_count = group->lut.end - group->lut.start;
    if (destination_count == 0u || destination_count > NMC_MAX_TILE_DESTINATIONS || core->output_queue_count >= NMC_MAX_OUTPUT_QUEUE) {
        return false;
    }

    NmcNetworkTile *tile = &core->output_queue[core->output_queue_count++];
    memset(tile, 0, sizeof(*tile));
    tile->destination_count = (uint8_t)destination_count;
    tile->width = group->width;
    memcpy(tile->payload, payload, payload_bytes(group->width));

    size_t destination_index = 0u;
    for (size_t i = group->lut.start; i < group->lut.end; ++i) {
        tile->destinations[destination_index++] = core->output_destination_lut[i];
    }
    return true;
}

static bool activate_output_group(NmcCore *core, NmcOutputGroup *group)
{
    uint8_t payload[NMC_MAX_GROUP_BYTES] = {0};
    for (nmc_tile_width_t i = 0; i < group->width; ++i) {
        if (group->neurons[i].accumulator >= group->neurons[i].threshold) {
            payload_set_bit(payload, i);
        }
        group->neurons[i].accumulator = 0;
    }
    group->remaining_input_count = group->input_count;
    return enqueue_network_tile(core, group, payload);
}

bool nmc_core_process_input_tile(NmcCore *core, const NmcInputTile *tile)
{
    if (!valid_input_index(core, tile->group_index)) {
        return false;
    }

    const NmcInputGroup *input_group = &core->input_groups[tile->group_index];
    if (!input_group->lut.valid || tile->width != input_group->width) {
        return false;
    }

    bool matched = false;
    for (size_t i = input_group->lut.start; i < input_group->lut.end; ++i) {
        NmcInputOutputPairLutEntry *pair_entry = &core->input_output_pair_lut[i];
        if (!valid_output_index(core, pair_entry->output_index)) {
            return false;
        }

        NmcOutputGroup *output_group = &core->output_groups[pair_entry->output_index];
        if (output_group->remaining_input_count == 0u) {
            return false;
        }

        accumulate_pair(core, input_group, pair_entry, output_group, tile->payload);
        --output_group->remaining_input_count;
        matched = true;

        if (output_group_ready(output_group) && !activate_output_group(core, output_group)) {
            return false;
        }
    }

    return matched;
}

bool nmc_core_pop_output_tile(NmcCore *core, NmcNetworkTile *tile)
{
    if (core->output_queue_count == 0u) {
        return false;
    }

    *tile = core->output_queue[0];
    memmove(&core->output_queue[0], &core->output_queue[1], (core->output_queue_count - 1u) * sizeof(core->output_queue[0]));
    --core->output_queue_count;
    return true;
}

bool nmc_network_tile_get_input_tile(const NmcNetworkTile *network_tile, size_t destination_index, NmcInputTile *input_tile)
{
    if (destination_index >= network_tile->destination_count) {
        return false;
    }

    input_tile->width = network_tile->width;
    input_tile->group_index = network_tile->destinations[destination_index].group_index;
    memcpy(input_tile->payload, network_tile->payload, payload_bytes(network_tile->width));
    return true;
}

void nmc_print_payload(const uint8_t *payload, nmc_tile_width_t width)
{
    putchar('0');
    putchar('b');
    for (int i = (int)width - 1; i >= 0; --i) {
        putchar(payload_bit_is_set(payload, (size_t)i) ? '1' : '0');
    }
}
