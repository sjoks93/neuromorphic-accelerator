#ifndef NEUROMORPHIC_ROUTER_H
#define NEUROMORPHIC_ROUTER_H

#include "neuromorphic_core.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * A small standalone router model for the next architecture stage.
 *
 * The router is intentionally independent from NmcCore.  It understands mesh
 * coordinates, destination lists, XY/YX next-hop selection, and multicast
 * splitting.  A later multi-core wrapper can connect the LOCAL port to a core
 * and the directional ports to neighboring routers.
 */
#define NMC_ROUTER_MAX_DESTINATIONS NMC_MAX_TILE_DESTINATIONS
#define NMC_ROUTER_MAX_PORT_QUEUE 16u

/* Router coordinates in a 2D mesh.  East/west change x; north/south change y. */
typedef struct {
    uint16_t x;
    uint16_t y;
} NmcMeshCoordinate;

/* The dimension order used to choose each destination's next hop. */
typedef enum {
    NMC_ROUTER_XY,
    NMC_ROUTER_YX,
} NmcRoutingOrder;

/* Output ports.  LOCAL means the message has reached this router's core. */
typedef enum {
    NMC_ROUTER_PORT_LOCAL = 0,
    NMC_ROUTER_PORT_EAST,
    NMC_ROUTER_PORT_WEST,
    NMC_ROUTER_PORT_NORTH,
    NMC_ROUTER_PORT_SOUTH,
    NMC_ROUTER_PORT_COUNT,
} NmcRouterPort;

/* One multicast destination: target router coordinate plus local core group. */
typedef struct {
    NmcMeshCoordinate coordinate;
    nmc_input_index_t group_index;
} NmcRouterDestination;

/* Router-facing message: width > 0 means tile payload, width == 0 means ACK. */
typedef struct {
    uint8_t destination_count;
    nmc_tile_width_t width;
    NmcRouterDestination destinations[NMC_ROUTER_MAX_DESTINATIONS];
    uint8_t payload[NMC_MAX_GROUP_BYTES];
    nmc_output_index_t completed_output_index;
} NmcRouterMessage;

typedef struct {
    NmcRouterMessage messages[NMC_ROUTER_MAX_PORT_QUEUE];
    size_t count;
} NmcRouterPortQueue;

/* Complete standalone router state. */
typedef struct {
    nmc_core_id_t core_id;
    NmcMeshCoordinate coordinate;
    NmcRoutingOrder routing_order;
    NmcRouterPortQueue output_queues[NMC_ROUTER_PORT_COUNT];
} NmcRouter;

void nmc_router_init(NmcRouter *router, nmc_core_id_t core_id, NmcMeshCoordinate coordinate, NmcRoutingOrder routing_order);
const char *nmc_router_port_name(NmcRouterPort port);
bool nmc_router_route_message(NmcRouter *router, const NmcRouterMessage *message);
bool nmc_router_pop_port_message(NmcRouter *router, NmcRouterPort port, NmcRouterMessage *message);
bool nmc_router_message_get_input_tile(const NmcRouterMessage *message, size_t destination_index, NmcInputTile *input_tile);
bool nmc_router_message_get_ack(const NmcRouter *router,
                                const NmcRouterMessage *message,
                                size_t destination_index,
                                NmcAckMessage *ack);

#endif