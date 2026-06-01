#include "neuromorphic_router.h"

#include <string.h>

static bool valid_router_port(NmcRouterPort port)
{
    return port < NMC_ROUTER_PORT_COUNT;
}

static bool valid_router_width(nmc_tile_width_t width)
{
    return width > 0u && width <= NMC_MAX_GROUP_NEURONS && (width % NMC_BITS_PER_BYTE) == 0u;
}

static size_t router_payload_bytes(nmc_tile_width_t width)
{
    return (size_t)width / NMC_BITS_PER_BYTE;
}

static bool coordinates_equal(NmcMeshCoordinate lhs, NmcMeshCoordinate rhs)
{
    return lhs.x == rhs.x && lhs.y == rhs.y;
}

static NmcRouterPort x_direction(NmcMeshCoordinate current, NmcMeshCoordinate target)
{
    return target.x > current.x ? NMC_ROUTER_PORT_EAST : NMC_ROUTER_PORT_WEST;
}

static NmcRouterPort y_direction(NmcMeshCoordinate current, NmcMeshCoordinate target)
{
    return target.y > current.y ? NMC_ROUTER_PORT_SOUTH : NMC_ROUTER_PORT_NORTH;
}

/*
 * Choose one next-hop port for one destination.
 *
 * XY routing resolves x first and then y.  YX routing resolves y first and
 * then x.  If both coordinates already match, the message is delivered to the
 * local core through the LOCAL port.
 */
static NmcRouterPort next_port_for_destination(const NmcRouter *router, NmcMeshCoordinate target)
{
    if (coordinates_equal(router->coordinate, target)) {
        return NMC_ROUTER_PORT_LOCAL;
    }

    if (router->routing_order == NMC_ROUTER_XY) {
        if (target.x != router->coordinate.x) {
            return x_direction(router->coordinate, target);
        }
        return y_direction(router->coordinate, target);
    }

    if (target.y != router->coordinate.y) {
        return y_direction(router->coordinate, target);
    }
    return x_direction(router->coordinate, target);
}

static bool validate_message(const NmcRouterMessage *message)
{
    if (message->destination_count == 0u || message->destination_count > NMC_ROUTER_MAX_DESTINATIONS) {
        return false;
    }

    return message->width == 0u || valid_router_width(message->width);
}

static bool append_destination(NmcRouterMessage *message, NmcRouterDestination destination)
{
    if (message->destination_count >= NMC_ROUTER_MAX_DESTINATIONS) {
        return false;
    }

    message->destinations[message->destination_count++] = destination;
    return true;
}

void nmc_router_init(NmcRouter *router, nmc_core_id_t core_id, NmcMeshCoordinate coordinate, NmcRoutingOrder routing_order)
{
    memset(router, 0, sizeof(*router));
    router->core_id = core_id;
    router->coordinate = coordinate;
    router->routing_order = routing_order;
}

const char *nmc_router_port_name(NmcRouterPort port)
{
    switch (port) {
    case NMC_ROUTER_PORT_LOCAL:
        return "LOCAL";
    case NMC_ROUTER_PORT_EAST:
        return "EAST";
    case NMC_ROUTER_PORT_WEST:
        return "WEST";
    case NMC_ROUTER_PORT_NORTH:
        return "NORTH";
    case NMC_ROUTER_PORT_SOUTH:
        return "SOUTH";
    case NMC_ROUTER_PORT_COUNT:
        break;
    }
    return "UNKNOWN";
}

bool nmc_router_route_message(NmcRouter *router, const NmcRouterMessage *message)
{
    if (!validate_message(message)) {
        return false;
    }

    /* Build one updated/split message per output port. */
    NmcRouterMessage split_messages[NMC_ROUTER_PORT_COUNT];
    memset(split_messages, 0, sizeof(split_messages));
    for (size_t port = 0u; port < NMC_ROUTER_PORT_COUNT; ++port) {
        split_messages[port].width = message->width;
        split_messages[port].completed_output_index = message->completed_output_index;
        memcpy(split_messages[port].payload, message->payload, sizeof(split_messages[port].payload));
    }

    for (uint8_t i = 0u; i < message->destination_count; ++i) {
        const NmcRouterDestination destination = message->destinations[i];
        const NmcRouterPort port = next_port_for_destination(router, destination.coordinate);
        if (!append_destination(&split_messages[port], destination)) {
            return false;
        }
    }

    /* Check queue space before mutating any output queue. */
    for (size_t port = 0u; port < NMC_ROUTER_PORT_COUNT; ++port) {
        if (split_messages[port].destination_count != 0u && router->output_queues[port].count >= NMC_ROUTER_MAX_PORT_QUEUE) {
            return false;
        }
    }

    for (size_t port = 0u; port < NMC_ROUTER_PORT_COUNT; ++port) {
        if (split_messages[port].destination_count != 0u) {
            NmcRouterPortQueue *queue = &router->output_queues[port];
            queue->messages[queue->count++] = split_messages[port];
        }
    }

    return true;
}

bool nmc_router_pop_port_message(NmcRouter *router, NmcRouterPort port, NmcRouterMessage *message)
{
    if (!valid_router_port(port)) {
        return false;
    }

    NmcRouterPortQueue *queue = &router->output_queues[port];
    if (queue->count == 0u) {
        return false;
    }

    *message = queue->messages[0];
    memmove(&queue->messages[0], &queue->messages[1], (queue->count - 1u) * sizeof(queue->messages[0]));
    --queue->count;
    return true;
}

bool nmc_router_message_get_input_tile(const NmcRouterMessage *message, size_t destination_index, NmcInputTile *input_tile)
{
    if (destination_index >= message->destination_count || !valid_router_width(message->width)) {
        return false;
    }

    input_tile->width = message->width;
    input_tile->group_index = message->destinations[destination_index].group_index;
    memcpy(input_tile->payload, message->payload, router_payload_bytes(message->width));
    return true;
}

bool nmc_router_message_get_ack(const NmcRouter *router,
                                const NmcRouterMessage *message,
                                size_t destination_index,
                                NmcAckMessage *ack)
{
    if (message->width != 0u || destination_index >= message->destination_count) {
        return false;
    }

    const NmcRouterDestination *destination = &message->destinations[destination_index];
    if (!coordinates_equal(router->coordinate, destination->coordinate)) {
        return false;
    }

    memset(ack, 0, sizeof(*ack));
    ack->destination_count = 1u;
    ack->destinations[0] = (NmcDestinationAddress){
        .core_id = router->core_id,
        .group_index = destination->group_index,
    };
    ack->completed_output_index = message->completed_output_index;
    return true;
}