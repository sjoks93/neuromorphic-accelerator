#include "nmc/internal/core.h"

#include <string.h>

bool nmc_core_output_group_ready(const NmcOutputGroup *group)
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
    if (!nmc_core_valid_output_index(core, output_index)) {
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

size_t nmc_core_output_group_successor_count(const NmcOutputGroup *group)
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

    const size_t destination_count = nmc_core_output_group_successor_count(group);
    if (destination_count == 0u || destination_count > NMC_MAX_TILE_DESTINATIONS || core->output_queue_count >= NMC_MAX_OUTPUT_QUEUE) {
        return false;
    }

    /* All successor ACKs from the previous emission must be observed before the next one. */
    return group->ack_count >= destination_count;
}

static bool enqueue_predecessor_acks(NmcCore *core, nmc_output_index_t output_index, bool recurrent)
{
    const NmcOutputRouteAddress *route_lut = &core->output_groups[output_index].route_lut;
    size_t predecessor_end = 0u;
    if (!output_group_predecessor_end(core, output_index, &predecessor_end)) {
        return false;
    }

    size_t predecessor_count = 0u;
    for (size_t i = route_lut->predecessor_start; i < predecessor_end; ++i) {
        if (core->output_route_lut[i].recurrent == recurrent) {
            ++predecessor_count;
        }
    }
    if (predecessor_count > NMC_MAX_TILE_DESTINATIONS) {
        return false;
    }
    if (predecessor_count == 0u) {
        return true;
    }
    if (core->ack_queue_count >= NMC_MAX_ACK_QUEUE) {
        return false;
    }

    /* One ACK message per completed output group, multicast to all predecessor edges. */
    NmcAckMessage *ack = &core->ack_queue[core->ack_queue_count++];
    memset(ack, 0, sizeof(*ack));
    ack->destination_count = (uint8_t)predecessor_count;
    ack->completed_output_index = output_index;

    size_t destination_index = 0u;
    for (size_t i = route_lut->predecessor_start; i < predecessor_end; ++i) {
        if (core->output_route_lut[i].recurrent == recurrent) {
            ack->destinations[destination_index++] = core->output_route_lut[i].address;
        }
    }

    return true;
}

static bool enqueue_recurrent_predecessor_acks_if_ready(NmcCore *core, nmc_output_index_t output_index)
{
    NmcOutputGroup *group = &core->output_groups[output_index];
    if (!nmc_core_output_group_ready(group)) {
        return false;
    }
    if (group->recurrent_ack_sent) {
        return true;
    }

    /* The first recurrent window is seeded with an implicit all-zero state, so
     * no real predecessor tile has been consumed and no recurrent credit is due. */
    if (group->primed_recurrent_input_count == 0u && !enqueue_predecessor_acks(core, output_index, true)) {
        return false;
    }
    group->recurrent_ack_sent = true;
    return true;
}

static bool enqueue_predecessor_acks_if_activating(NmcCore *core, nmc_output_index_t output_index)
{
    NmcOutputGroup *group = &core->output_groups[output_index];
    if (group->predecessor_ack_sent) {
        return true;
    }
    if (!enqueue_predecessor_acks(core, output_index, false)) {
        return false;
    }
    group->predecessor_ack_sent = true;
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
    memcpy(tile->payload, payload, nmc_core_payload_bytes(group->route_lut.bitmap_width));

    size_t destination_index = 0u;
    for (size_t i = group->route_lut.successor_start; i < group->route_lut.predecessor_start; ++i) {
        tile->destinations[destination_index++] = core->output_route_lut[i].address;
    }
    return true;
}

bool nmc_core_activate_output_group(NmcCore *core, nmc_output_index_t output_index)
{
    NmcOutputGroup *group = &core->output_groups[output_index];
    if (!nmc_core_output_group_ready(group) ||
        !enqueue_recurrent_predecessor_acks_if_ready(core, output_index) ||
        !output_group_destinations_ready(core, output_index)) {
        return false;
    }

    size_t predecessor_end = 0u;
    if (!output_group_predecessor_end(core, output_index, &predecessor_end)) {
        return false;
    }
    const size_t predecessor_count = predecessor_end - group->route_lut.predecessor_start;
    if (predecessor_count > NMC_MAX_TILE_DESTINATIONS) {
        return false;
    }

    size_t accumulator_start = 0u;
    if (!nmc_core_output_accumulators_fit(core, output_index, &accumulator_start)) {
        return false;
    }
    if (!enqueue_predecessor_acks_if_activating(core, output_index)) {
        return false;
    }

    uint8_t payload[NMC_MAX_GROUP_BYTES] = {0};
    for (nmc_tile_width_t i = 0; i < group->route_lut.bitmap_width; ++i) {
        int32_t accumulator = 0;
        if (!nmc_core_memory_read_accumulator(core,
                                              accumulator_start,
                                              i,
                                              NMC_MEMORY_OUTPUT_TO_ACCUMULATOR_MUX,
                                              &accumulator)) {
            return false;
        }
        if (accumulator >= group->neurons[i].threshold) {
            nmc_core_payload_set_bit(payload, i);
        }
        if (!nmc_core_memory_write_accumulator(core, accumulator_start, i, 0)) {
            return false;
        }
    }
    if (!enqueue_network_tile(core, output_index, payload)) {
        return false;
    }

    /* Start the next synchronization window. */
    group->input_count = 0u;
    group->ack_count = 0u;
    group->primed_recurrent_input_count = 0u;
    group->recurrent_ack_sent = false;
    group->predecessor_ack_sent = false;
    return true;
}

bool nmc_core_flush_ready_outputs(NmcCore *core)
{
    bool emitted = false;
    for (nmc_output_index_t output_index = 0u; output_index < core->output_group_count; ++output_index) {
        if (nmc_core_output_group_ready(&core->output_groups[output_index])) {
            if (!enqueue_recurrent_predecessor_acks_if_ready(core, output_index)) {
                continue;
            }
            emitted = nmc_core_activate_output_group(core, output_index) || emitted;
        }
    }
    return emitted;
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
    memcpy(input_tile->payload, network_tile->payload, nmc_core_payload_bytes(network_tile->width));
    return true;
}
