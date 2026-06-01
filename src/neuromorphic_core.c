#include "neuromorphic_core.h"

#include <stdio.h>
#include <string.h>

/* Validate destination-local input group indices. */
static bool valid_input_index(const NmcCore *core, nmc_input_index_t input_index)
{
    return input_index < core->input_group_count;
}

/* Validate destination-local ACK group indices. */
static bool valid_ack_index(const NmcCore *core, nmc_ack_index_t ack_index)
{
    return ack_index < core->ack_group_count;
}

static bool valid_output_index(const NmcCore *core, nmc_output_index_t output_index)
{
    return output_index < core->output_group_count;
}

/* Width is carried in tile headers and must map cleanly onto bytes. */
static bool valid_width(nmc_tile_width_t width)
{
    return width > 0u && width <= NMC_MAX_GROUP_NEURONS && (width % NMC_BITS_PER_BYTE) == 0u;
}

static size_t payload_bytes(nmc_tile_width_t width)
{
    return (size_t)width / NMC_BITS_PER_BYTE;
}

/* Payload bits are stored little-endian inside each byte. */
static bool payload_bit_is_set(const uint8_t *payload, size_t bit_index)
{
    return (payload[bit_index / NMC_BITS_PER_BYTE] & (uint8_t)(UINT8_C(1) << (bit_index % NMC_BITS_PER_BYTE))) != 0u;
}

static void payload_set_bit(uint8_t *payload, size_t bit_index)
{
    payload[bit_index / NMC_BITS_PER_BYTE] |= (uint8_t)(UINT8_C(1) << (bit_index % NMC_BITS_PER_BYTE));
}

/* Check that the contiguous input/output weight slice is inside weight SRAM. */
static bool pair_weights_fit(const NmcCore *core, nmc_tile_width_t input_width, const NmcInputOutputPairLutEntry *pair_entry)
{
    if (!valid_width(input_width) || !valid_output_index(core, pair_entry->output_index)) {
        return false;
    }

    const size_t output_width = core->output_groups[pair_entry->output_index].route_lut.bitmap_width;
    const size_t pair_weights = (size_t)input_width * output_width;
    return pair_entry->weight_offset <= core->weight_count && pair_weights <= core->weight_count - pair_entry->weight_offset;
}

void nmc_core_init(NmcCore *core, nmc_core_id_t core_id, int16_t *weights, size_t weight_count)
{
    memset(core, 0, sizeof(*core));
    core->core_id = core_id;
    core->weights = weights;
    core->weight_count = weight_count;
}

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
    if (!valid_width(width) || core->output_group_count >= NMC_MAX_OUTPUT_GROUPS) {
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
                                       nmc_input_index_t group_index)
{
    if (core->output_route_lut_count >= NMC_MAX_OUTPUT_ROUTE_LUT_ENTRIES) {
        return false;
    }

    core->output_route_lut[core->output_route_lut_count++] = (NmcOutputRouteLutEntry){
        .address = {
            .core_id = core_id,
            .group_index = group_index,
        },
    };
    return true;
}

bool nmc_core_add_output_successor_lut_entry(NmcCore *core,
                                             nmc_core_id_t target_core_id,
                                             nmc_input_index_t target_group_index)
{
    return add_output_route_lut_entry(core, target_core_id, target_group_index);
}

bool nmc_core_add_output_predecessor_lut_entry(NmcCore *core,
                                               nmc_core_id_t predecessor_core_id,
                                               nmc_input_index_t predecessor_group_index)
{
    return add_output_route_lut_entry(core, predecessor_core_id, predecessor_group_index);
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
    ++output_group->input_requirement;
    return true;
}

bool nmc_core_add_ack_output_pair_lut_entry(NmcCore *core,
                                            nmc_output_index_t output_index)
{
    /* Stage-2 ACK LUT entry: ACK index -> output group ACK counter. */
    if (core->ack_output_pair_lut_count >= NMC_MAX_ACK_OUTPUT_PAIR_LUT_ENTRIES) {
        return false;
    }

    if (!valid_output_index(core, output_index)) {
        return false;
    }

    core->ack_output_pair_lut[core->ack_output_pair_lut_count++] = (NmcAckOutputPairLutEntry){
        .output_index = output_index,
    };
    return true;
}

/* Simplified compute: active input bits select columns from the weight matrix. */
static void accumulate_pair(NmcCore *core,
                            nmc_tile_width_t input_width,
                            const NmcInputOutputPairLutEntry *pair_entry,
                            NmcOutputGroup *output_group,
                            const uint8_t *input_payload)
{
    for (size_t out = 0; out < output_group->route_lut.bitmap_width; ++out) {
        int32_t sum = 0;
        for (size_t in = 0; in < input_width; ++in) {
            if (payload_bit_is_set(input_payload, in)) {
                const size_t weight_index = pair_entry->weight_offset + out * input_width + in;
                sum += core->weights[weight_index];
            }
        }
        output_group->neurons[out].accumulator += sum;
    }
}

static bool output_group_ready(const NmcOutputGroup *group)
{
    return group->input_requirement != 0u && group->input_count == group->input_requirement;
}

/*
 * Derive the predecessor exclusive end without storing a third pointer.
 *
 * Route LUT layout is:
 *   successors(G0), predecessors(G0), successors(G1), predecessors(G1), ...
 * Therefore predecessors(Gi) end at successor_start(Gi + 1).  For the last
 * output group, Gi + 1 is the terminal output-table entry configured by the
 * test bench or mapper.
 */
static bool output_group_predecessor_end(const NmcCore *core, nmc_output_index_t output_index, size_t *predecessor_end)
{
    if (!valid_output_index(core, output_index)) {
        return false;
    }

    const NmcOutputRouteAddress *route_lut = &core->output_groups[output_index].route_lut;
    if (!route_lut->valid) {
        return false;
    }

    const NmcOutputRouteAddress *next_route_lut = &core->output_groups[output_index + 1u].route_lut;
    if (!next_route_lut->valid) {
        return false;
    }
    *predecessor_end = next_route_lut->successor_start;

    return route_lut->predecessor_start <= *predecessor_end && *predecessor_end <= core->output_route_lut_count;
}

static size_t output_group_predecessor_count(const NmcCore *core, nmc_output_index_t output_index)
{
    size_t predecessor_end = 0u;
    if (!output_group_predecessor_end(core, output_index, &predecessor_end)) {
        return 0u;
    }

    return predecessor_end - core->output_groups[output_index].route_lut.predecessor_start;
}

static size_t output_group_successor_count(const NmcOutputGroup *group)
{
    return group->route_lut.predecessor_start - group->route_lut.successor_start;
}

static bool output_group_destinations_ready(const NmcCore *core, nmc_output_index_t output_index)
{
    const NmcOutputGroup *group = &core->output_groups[output_index];
    if (!group->route_lut.valid) {
        return false;
    }

    /* Validate the predecessor end even though this function only scans successors. */
    size_t predecessor_end = 0u;
    if (!output_group_predecessor_end(core, output_index, &predecessor_end)) {
        return false;
    }

    const size_t destination_count = output_group_successor_count(group);
    if (destination_count == 0u || destination_count > NMC_MAX_TILE_DESTINATIONS || core->output_queue_count >= NMC_MAX_OUTPUT_QUEUE) {
        return false;
    }

    /* All successor ACKs from the previous emission must be observed before the next one. */
    return group->ack_count >= destination_count;
}

static bool enqueue_predecessor_acks(NmcCore *core, nmc_output_index_t output_index)
{
    const NmcOutputRouteAddress *route_lut = &core->output_groups[output_index].route_lut;
    const size_t predecessor_count = output_group_predecessor_count(core, output_index);
    size_t predecessor_end = 0u;
    if (!output_group_predecessor_end(core, output_index, &predecessor_end)) {
        return false;
    }
    if (predecessor_count > NMC_MAX_ACK_QUEUE - core->ack_queue_count) {
        return false;
    }

    /* ACKs release predecessor edges for their next natural step. */
    for (size_t i = route_lut->predecessor_start; i < predecessor_end; ++i) {
        core->ack_queue[core->ack_queue_count++] = (NmcAckMessage){
            .destination = core->output_route_lut[i].address,
            .completed_output_index = output_index,
        };
    }

    return true;
}

static bool enqueue_network_tile(NmcCore *core, nmc_output_index_t output_index, const uint8_t *payload)
{
    if (!output_group_destinations_ready(core, output_index)) {
        return false;
    }

    const NmcOutputGroup *group = &core->output_groups[output_index];
    NmcNetworkTile *tile = &core->output_queue[core->output_queue_count++];
    memset(tile, 0, sizeof(*tile));
    tile->destination_count = (uint8_t)(group->route_lut.predecessor_start - group->route_lut.successor_start);
    tile->width = group->route_lut.bitmap_width;
    memcpy(tile->payload, payload, payload_bytes(group->route_lut.bitmap_width));

    /* Copy multicast destinations.  Per-output ACK sync is reset after injection. */
    size_t destination_index = 0u;
    for (size_t i = group->route_lut.successor_start; i < group->route_lut.predecessor_start; ++i) {
        tile->destinations[destination_index++] = core->output_route_lut[i].address;
    }
    return true;
}

static bool activate_output_group(NmcCore *core, nmc_output_index_t output_index)
{
    NmcOutputGroup *group = &core->output_groups[output_index];
    if (!output_group_destinations_ready(core, output_index)) {
        return false;
    }

    size_t predecessor_end = 0u;
    if (!output_group_predecessor_end(core, output_index, &predecessor_end)) {
        return false;
    }
    const size_t predecessor_count = predecessor_end - group->route_lut.predecessor_start;
    if (predecessor_count > NMC_MAX_ACK_QUEUE - core->ack_queue_count) {
        return false;
    }

    /* Threshold accumulated neuron values into the outgoing spike bitmap. */
    uint8_t payload[NMC_MAX_GROUP_BYTES] = {0};
    for (nmc_tile_width_t i = 0; i < group->route_lut.bitmap_width; ++i) {
        if (group->neurons[i].accumulator >= group->neurons[i].threshold) {
            payload_set_bit(payload, i);
        }
        group->neurons[i].accumulator = 0;
    }
    if (!enqueue_network_tile(core, output_index, payload)) {
        return false;
    }
    if (!enqueue_predecessor_acks(core, output_index)) {
        return false;
    }

    /*
     * Start the next synchronization window.  Predecessor ACKs are generated
     * immediately because this core has consumed the inputs that produced the
     * tile.  Successor ACKs from the just-emitted tile will later unlock the
     * next output emission.
     */
    group->input_count = 0u;
    group->ack_count = 0u;
    return true;
}

bool nmc_core_flush_ready_outputs(NmcCore *core)
{
    /* Called after ACKs arrive to release outputs that were complete but blocked. */
    bool emitted = false;
    for (nmc_output_index_t output_index = 0u; output_index < core->output_group_count; ++output_index) {
        if (output_group_ready(&core->output_groups[output_index])) {
            emitted = activate_output_group(core, output_index) || emitted;
        }
    }
    return emitted;
}

bool nmc_core_process_input_tile(NmcCore *core, const NmcInputTile *tile)
{
    if (!valid_input_index(core, tile->group_index) || !valid_width(tile->width)) {
        return false;
    }

    /* The following input-group entry provides the exclusive end of this range. */
    const NmcInputGroup *input_group = &core->input_groups[tile->group_index];
    const NmcInputGroup *next_input_group = &core->input_groups[tile->group_index + 1u];
    if (!input_group->lut.valid || !next_input_group->lut.valid || input_group->lut.start > next_input_group->lut.start) {
        return false;
    }

    /* Walk all output groups subscribed to this input group. */
    bool matched = false;
    for (size_t i = input_group->lut.start; i < next_input_group->lut.start; ++i) {
        NmcInputOutputPairLutEntry *pair_entry = &core->input_output_pair_lut[i];
        if (!valid_output_index(core, pair_entry->output_index)) {
            return false;
        }
        if (!pair_weights_fit(core, tile->width, pair_entry)) {
            return false;
        }

        NmcOutputGroup *output_group = &core->output_groups[pair_entry->output_index];
        if (output_group->input_count >= output_group->input_requirement) {
            return false;
        }

        /* One arrival contributes once to this output group's current step. */
        accumulate_pair(core, tile->width, pair_entry, output_group, tile->payload);
        ++output_group->input_count;
        matched = true;

        if (output_group_ready(output_group)) {
            (void)activate_output_group(core, pair_entry->output_index);
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

bool nmc_core_process_ack(NmcCore *core, const NmcAckMessage *ack)
{
    /* The router has selected this core; the local group field is the ACK index. */
    const nmc_ack_index_t ack_index = (nmc_ack_index_t)ack->destination.group_index;
    if (!valid_ack_index(core, ack_index)) {
        return false;
    }

    /* The following ACK-group entry provides the exclusive end of this range. */
    const NmcAckGroup *ack_group = &core->ack_groups[ack_index];
    const NmcAckGroup *next_ack_group = &core->ack_groups[ack_index + 1u];
    if (!ack_group->lut.valid || !next_ack_group->lut.valid || ack_group->lut.start > next_ack_group->lut.start) {
        return false;
    }

    bool matched = false;
    for (size_t i = ack_group->lut.start; i < next_ack_group->lut.start; ++i) {
        const NmcAckOutputPairLutEntry *pair_entry = &core->ack_output_pair_lut[i];
        if (!valid_output_index(core, pair_entry->output_index)) {
            return false;
        }

        NmcOutputGroup *output_group = &core->output_groups[pair_entry->output_index];
        if (output_group->ack_count == UINT32_MAX) {
            return false;
        }

        const size_t successor_count = output_group_successor_count(output_group);
        if (successor_count == 0u || output_group->ack_count >= successor_count) {
            return false;
        }

        /* Returning an ACK may unblock a previously complete output group. */
        ++output_group->ack_count;
        matched = true;
    }

    (void)nmc_core_flush_ready_outputs(core);
    return matched;
}

bool nmc_core_pop_ack(NmcCore *core, NmcAckMessage *ack)
{
    if (core->ack_queue_count == 0u) {
        return false;
    }

    *ack = core->ack_queue[0];
    memmove(&core->ack_queue[0], &core->ack_queue[1], (core->ack_queue_count - 1u) * sizeof(core->ack_queue[0]));
    --core->ack_queue_count;
    return true;
}

bool nmc_network_tile_get_input_tile(const NmcNetworkTile *network_tile, size_t destination_index, NmcInputTile *input_tile)
{
    if (destination_index >= network_tile->destination_count) {
        return false;
    }

    /* Routers strip global destination headers before presenting a local tile. */
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
