#include "neuromorphic_core.h"
#include "neuromorphic_router.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Tiny self-checking test-bench macro.
 *
 * The simulator is intentionally dependency-free, so this file doubles as both
 * an executable example and a regression test for the core interface.
 */
#define CHECK(call) do { \
    if (!(call)) { \
        fprintf(stderr, "configuration or runtime error: %s\n", #call); \
        return EXIT_FAILURE; \
    } \
} while (0)

/* Keep route indices symbolic so output routing tests are readable. */
enum {
    ROUTE_X_TO_CORE_1 = 0,
    ROUTE_X_TO_CORE_2 = 1,
    ROUTE_X_ACK_TO_PREDECESSOR_A = 2,
    ROUTE_X_ACK_TO_PREDECESSOR_B = 3,
    ROUTE_Y_TO_CORE_1 = 4,
    ROUTE_Y_TO_CORE_3 = 5,
    ROUTE_Y_ACK_TO_PREDECESSOR_A = 6,
    ROUTE_TERMINAL = 7,
};

/* Incoming ACK group indices are a separate namespace from input tile groups. */
enum {
    ACK_FROM_CORE_1 = 0,
    ACK_FROM_CORE_2 = 1,
    ACK_FROM_CORE_3 = 2,
    ACK_TERMINAL = 3,
};

/* Print one router-facing multicast tile. */
static void print_network_tile(const NmcNetworkTile *tile)
{
    printf("network tile destinations=%u width=%u payload=", tile->destination_count, tile->width);
    nmc_print_payload(tile->payload, tile->width);
    printf(" destination_addresses=[");
    for (uint8_t i = 0; i < tile->destination_count; ++i) {
        printf("%s{core_id=%u, group_index=%u}",
               i == 0u ? "" : ", ",
               tile->destinations[i].core_id,
               tile->destinations[i].group_index);
    }
    puts("]");
}

/* Decode the same network tile as each destination core would see it. */
static void print_router_delivery(const NmcNetworkTile *tile)
{
    for (uint8_t i = 0; i < tile->destination_count; ++i) {
        NmcInputTile input_tile;
        if (!nmc_network_tile_get_input_tile(tile, i, &input_tile)) {
            puts("  router delivery decode failed");
            continue;
        }
        printf("  router delivers to core_id=%u as input group_index=%u payload=",
               tile->destinations[i].core_id,
               input_tile.group_index);
        nmc_print_payload(input_tile.payload, input_tile.width);
        putchar('\n');
    }
}

/* Drain output queue and return how many network tiles were produced. */
static size_t drain_outputs(NmcCore *core)
{
    size_t count = 0u;
    NmcNetworkTile tile;
    while (nmc_core_pop_output_tile(core, &tile)) {
        ++count;
        print_network_tile(&tile);
        print_router_delivery(&tile);
    }
    if (count == 0u) {
        puts("network tile queue empty");
    }
    return count;
}

/* Drain ACK queue and return how many multicast predecessor ACK messages were generated. */
static size_t drain_acks(NmcCore *core)
{
    size_t count = 0u;
    NmcAckMessage ack;
    while (nmc_core_pop_ack(core, &ack)) {
        ++count;
        printf("ack destinations=%u completed_output_index=%u destination_addresses=[", ack.destination_count, ack.completed_output_index);
        for (uint8_t i = 0u; i < ack.destination_count; ++i) {
            printf("%s{core_id=%u, group_index=%u}",
                   i == 0u ? "" : ", ",
                   ack.destinations[i].core_id,
                   ack.destinations[i].group_index);
        }
        puts("]");
    }
    if (count == 0u) {
        puts("ack queue empty");
    }
    return count;
}

/* Print the two runtime counters used by each output group. */
static void print_output_counters(const NmcCore *core)
{
    const NmcOutputGroup *output_x = &core->output_groups[0];
    const NmcOutputGroup *output_y = &core->output_groups[1];
    printf("output counters: X inputs=%u/%u acks=%u/2, Y inputs=%u/%u acks=%u/2\n",
           output_x->input_count,
           output_x->input_requirement,
           output_x->ack_count,
           output_y->input_count,
           output_y->input_requirement,
           output_y->ack_count);
}

/* Print the event-driven compute schedule counters. */
static void print_compute_counters(const NmcCore *core)
{
    printf("compute schedule: events=%u encoder_cycles=%llu output_parallelism=%u input_parallelism=%u last_input_tile_compute_cycles=%llu total_compute_cycles=%llu\n",
           core->last_input_tile_event_count,
           (unsigned long long)core->last_input_tile_encoder_cycles,
           core->output_parallelism,
           core->input_parallelism,
           (unsigned long long)core->last_input_tile_compute_cycles,
           (unsigned long long)core->total_compute_cycles);
}

/* Print the input event that is about to be consumed by the core. */
static void print_input_tile(const NmcInputTile *tile)
{
    printf("processing input group_index=%u width=%u payload=", tile->group_index, tile->width);
    nmc_print_payload(tile->payload, tile->width);
    putchar('\n');
}

/* Process one router-delivered local ACK message through the ACK input LUT. */
static bool process_successor_ack(NmcCore *core, nmc_ack_index_t ack_index)
{
    const NmcAckMessage ack = {
        .destination_count = 1u,
        .destinations = {
            {
                .core_id = core->core_id,
                .group_index = ack_index,
            },
        },
        .completed_output_index = NMC_INVALID_INDEX,
    };

    printf("processing incoming successor ack group_index=%u\n", ack_index);
    return nmc_core_process_ack(core, &ack);
}

/* Visual separator for the multi-step trace. */
static void begin_step(const char *title)
{
    printf("\n=== %s ===\n", title);
}

/* Print one router-produced forwarded message. */
static void print_router_message(NmcRouterPort port, const NmcRouterMessage *message)
{
    printf("router port %s message destinations=%u", nmc_router_port_name(port), message->destination_count);
    if (message->width != 0u) {
        printf(" width=%u payload=", message->width);
        nmc_print_payload(message->payload, message->width);
    } else {
        printf(" ack completed_output_index=%u", message->completed_output_index);
    }
    printf(" destination_coordinates=[");
    for (uint8_t i = 0u; i < message->destination_count; ++i) {
        printf("%s{x=%u, y=%u, group_index=%u}",
               i == 0u ? "" : ", ",
               message->destinations[i].coordinate.x,
               message->destinations[i].coordinate.y,
               message->destinations[i].group_index);
    }
    puts("]");
}

/* Drain one router port and check both message count and routed destination count. */
static bool drain_router_port_expect(NmcRouter *router,
                                     NmcRouterPort port,
                                     size_t expected_messages,
                                     size_t expected_destinations)
{
    size_t messages = 0u;
    size_t destinations = 0u;
    NmcRouterMessage message;
    while (nmc_router_pop_port_message(router, port, &message)) {
        ++messages;
        destinations += message.destination_count;
        print_router_message(port, &message);

        if (port == NMC_ROUTER_PORT_LOCAL && message.width != 0u) {
            for (uint8_t i = 0u; i < message.destination_count; ++i) {
                NmcInputTile input_tile;
                if (!nmc_router_message_get_input_tile(&message, i, &input_tile)) {
                    return false;
                }
                printf("  local core receives input group_index=%u payload=", input_tile.group_index);
                nmc_print_payload(input_tile.payload, input_tile.width);
                putchar('\n');
            }
        }
    }
    if (messages == 0u) {
        printf("router port %s queue empty\n", nmc_router_port_name(port));
    }

    return messages == expected_messages && destinations == expected_destinations;
}

/* Test-bench-only map from core IDs used by core route LUTs to mesh coordinates used by routers. */
typedef struct {
    nmc_core_id_t core_id;
    NmcMeshCoordinate coordinate;
} NmcCoreMeshMapEntry;

static bool lookup_core_coordinate(const NmcCoreMeshMapEntry *map,
                                   size_t map_count,
                                   nmc_core_id_t core_id,
                                   NmcMeshCoordinate *coordinate)
{
    for (size_t i = 0u; i < map_count; ++i) {
        if (map[i].core_id == core_id) {
            *coordinate = map[i].coordinate;
            return true;
        }
    }
    return false;
}

/* Convert a core-generated tile into the coordinate-addressed format consumed by routers. */
static bool network_tile_to_router_message(const NmcNetworkTile *tile,
                                           const NmcCoreMeshMapEntry *map,
                                           size_t map_count,
                                           NmcRouterMessage *message)
{
    memset(message, 0, sizeof(*message));
    message->destination_count = tile->destination_count;
    message->width = tile->width;
    memcpy(message->payload, tile->payload, sizeof(message->payload));

    for (uint8_t i = 0u; i < tile->destination_count; ++i) {
        if (!lookup_core_coordinate(map, map_count, tile->destinations[i].core_id, &message->destinations[i].coordinate)) {
            return false;
        }
        message->destinations[i].group_index = tile->destinations[i].group_index;
    }
    return true;
}

/* Convert a core-generated ACK into a coordinate-addressed router message. */
static bool ack_to_router_message(const NmcAckMessage *ack,
                                  const NmcCoreMeshMapEntry *map,
                                  size_t map_count,
                                  NmcRouterMessage *message)
{
    if (ack->destination_count == 0u || ack->destination_count > NMC_ROUTER_MAX_DESTINATIONS) {
        return false;
    }

    memset(message, 0, sizeof(*message));
    message->width = 0u;
    message->destination_count = ack->destination_count;
    message->completed_output_index = ack->completed_output_index;
    for (uint8_t i = 0u; i < ack->destination_count; ++i) {
        if (!lookup_core_coordinate(map, map_count, ack->destinations[i].core_id, &message->destinations[i].coordinate)) {
            return false;
        }
        message->destinations[i].group_index = ack->destinations[i].group_index;
    }
    return true;
}

/* Inject all pending core output tiles into that core's local router. */
static bool inject_core_outputs_into_router_expect(NmcCore *core,
                                                   NmcRouter *router,
                                                   const NmcCoreMeshMapEntry *map,
                                                   size_t map_count,
                                                   size_t expected_tiles)
{
    size_t count = 0u;
    NmcNetworkTile tile;
    while (nmc_core_pop_output_tile(core, &tile)) {
        NmcRouterMessage message;
        ++count;
        printf("core %u injects output tile into router at (%u,%u)\n", core->core_id, router->coordinate.x, router->coordinate.y);
        if (!network_tile_to_router_message(&tile, map, map_count, &message) || !nmc_router_route_message(router, &message)) {
            return false;
        }
    }
    if (count == 0u) {
        printf("core %u output queue empty\n", core->core_id);
    }
    return count == expected_tiles;
}

/* Inject all pending core ACKs into that core's local router. */
static bool inject_core_acks_into_router_expect(NmcCore *core,
                                                NmcRouter *router,
                                                const NmcCoreMeshMapEntry *map,
                                                size_t map_count,
                                                size_t expected_acks)
{
    size_t count = 0u;
    NmcAckMessage ack;
    while (nmc_core_pop_ack(core, &ack)) {
        NmcRouterMessage message;
        ++count;
        printf("core %u injects ACK into router at (%u,%u)\n", core->core_id, router->coordinate.x, router->coordinate.y);
        if (!ack_to_router_message(&ack, map, map_count, &message) || !nmc_router_route_message(router, &message)) {
            return false;
        }
    }
    if (count == 0u) {
        printf("core %u ACK queue empty\n", core->core_id);
    }
    return count == expected_acks;
}

/* Move all messages from one router port into the neighboring router's input. */
static bool transfer_router_port_expect(NmcRouter *from,
                                        NmcRouterPort port,
                                        NmcRouter *to,
                                        size_t expected_messages,
                                        size_t expected_destinations)
{
    size_t messages = 0u;
    size_t destinations = 0u;
    NmcRouterMessage message;
    while (nmc_router_pop_port_message(from, port, &message)) {
        ++messages;
        destinations += message.destination_count;
        printf("mesh transfers %s from (%u,%u) to (%u,%u)\n",
               nmc_router_port_name(port),
               from->coordinate.x,
               from->coordinate.y,
               to->coordinate.x,
               to->coordinate.y);
        print_router_message(port, &message);
        if (!nmc_router_route_message(to, &message)) {
            return false;
        }
    }
    if (messages == 0u) {
        printf("router port %s at (%u,%u) queue empty\n", nmc_router_port_name(port), from->coordinate.x, from->coordinate.y);
    }
    return messages == expected_messages && destinations == expected_destinations;
}

/* Deliver LOCAL router messages to the attached core. */
static bool deliver_router_local_to_core_expect(NmcRouter *router,
                                                NmcCore *core,
                                                size_t expected_tiles,
                                                size_t expected_acks)
{
    size_t tiles = 0u;
    size_t acks = 0u;
    NmcRouterMessage message;
    while (nmc_router_pop_port_message(router, NMC_ROUTER_PORT_LOCAL, &message)) {
        print_router_message(NMC_ROUTER_PORT_LOCAL, &message);
        for (uint8_t i = 0u; i < message.destination_count; ++i) {
            if (message.width != 0u) {
                NmcInputTile input_tile;
                if (!nmc_router_message_get_input_tile(&message, i, &input_tile)) {
                    return false;
                }
                ++tiles;
                printf("router at (%u,%u) delivers tile to core %u input group_index=%u payload=",
                       router->coordinate.x,
                       router->coordinate.y,
                       core->core_id,
                       input_tile.group_index);
                nmc_print_payload(input_tile.payload, input_tile.width);
                putchar('\n');
                if (!nmc_core_process_input_tile(core, &input_tile)) {
                    return false;
                }
            } else {
                NmcAckMessage ack;
                if (!nmc_router_message_get_ack(router, &message, i, &ack)) {
                    return false;
                }
                ++acks;
                printf("router at (%u,%u) delivers ACK to core %u ack group_index=%u\n",
                       router->coordinate.x,
                       router->coordinate.y,
                       core->core_id,
                       ack.destinations[0].group_index);
                if (!nmc_core_process_ack(core, &ack)) {
                    return false;
                }
            }
        }
    }
    if (tiles == 0u && acks == 0u) {
        printf("router LOCAL port at (%u,%u) queue empty\n", router->coordinate.x, router->coordinate.y);
    }
    return tiles == expected_tiles && acks == expected_acks;
}

static void fill_weights(int16_t *weights, size_t weight_count)
{
    for (size_t i = 0u; i < weight_count; ++i) {
        weights[i] = 1;
    }
}

int main(void)
{
    enum {
        CORE_ID = 0,
        INPUT_A = 0,
        INPUT_B = 1,
        PREDECESSOR_A_ACK_INDEX = 0,
        PREDECESSOR_B_ACK_INDEX = 1,
        OUTPUT_X = 0,
        OUTPUT_Y = 1,
        INPUT_WIDTH = 8,
        OUTPUT_X_WIDTH = 8,
        OUTPUT_Y_WIDTH = 8,
        OUTPUT_X_INPUT_A_OFFSET = 0,
        OUTPUT_X_INPUT_B_OFFSET = OUTPUT_X_INPUT_A_OFFSET + OUTPUT_X_WIDTH * INPUT_WIDTH,
        OUTPUT_Y_INPUT_A_OFFSET = OUTPUT_X_INPUT_B_OFFSET + OUTPUT_X_WIDTH * INPUT_WIDTH,
        WEIGHT_COUNT = OUTPUT_Y_INPUT_A_OFFSET + OUTPUT_Y_WIDTH * INPUT_WIDTH,
    };

    /*
     * Contiguous weight memory:
     *   offset   0: OUTPUT_X x INPUT_A, 8 outputs * 8 inputs
     *   offset  64: OUTPUT_X x INPUT_B, 8 outputs * 8 inputs
     *   offset 128: OUTPUT_Y x INPUT_A, 8 outputs * 8 inputs
     *
     * All weights are one, so each output neuron sees the input popcount.
     */
    int16_t weights[WEIGHT_COUNT];
    for (size_t i = 0; i < WEIGHT_COUNT; ++i) {
        weights[i] = 1;
    }

    /* Create one core under test. */
    NmcCore core;
    nmc_core_init(&core, CORE_ID, weights, sizeof(weights) / sizeof(weights[0]));
    CHECK(nmc_core_set_output_parallelism(&core, 2u));
    CHECK(nmc_core_set_input_parallelism(&core, 2u));

    /* Two input groups: A contributes to X and Y; B contributes only to X. */
    CHECK(nmc_core_add_input_group(&core));
    CHECK(nmc_core_add_input_group(&core));

    /* Three ACK groups: core 1 is a common successor shared by X and Y. */
    CHECK(nmc_core_add_ack_group(&core));
    CHECK(nmc_core_add_ack_group(&core));
    CHECK(nmc_core_add_ack_group(&core));

    /* X needs both inputs; Y needs only A. */
    const int32_t thresholds_x[] = {2, 2, 2, 2, 2, 2, 2, 2};
    const int32_t thresholds_y[] = {1, 1, 1, 1, 1, 1, 1, 1};
    CHECK(nmc_core_add_output_group(&core, OUTPUT_X_WIDTH, thresholds_x));
    CHECK(nmc_core_add_output_group(&core, OUTPUT_Y_WIDTH, thresholds_y));

    /*
     * Stage-2 output route LUT, ordered as:
     *   successors(X), predecessors(X), successors(Y), predecessors(Y)
     */
    CHECK(nmc_core_add_output_successor_lut_entry(&core, 1, 0));
    CHECK(nmc_core_add_output_successor_lut_entry(&core, 2, 0));
    CHECK(nmc_core_add_output_predecessor_lut_entry(&core, CORE_ID, PREDECESSOR_A_ACK_INDEX));
    CHECK(nmc_core_add_output_predecessor_lut_entry(&core, CORE_ID, PREDECESSOR_B_ACK_INDEX));
    CHECK(nmc_core_add_output_successor_lut_entry(&core, 1, 0));
    CHECK(nmc_core_add_output_successor_lut_entry(&core, 3, 1));
    CHECK(nmc_core_add_output_predecessor_lut_entry(&core, CORE_ID, PREDECESSOR_A_ACK_INDEX));

    /*
     * Stage-1 output starts.  The terminal entry at index 2 provides the
     * exclusive end for OUTPUT_Y's predecessor range.
     */
    CHECK(nmc_core_set_output_lut_starts(&core, OUTPUT_X, ROUTE_X_TO_CORE_1, ROUTE_X_ACK_TO_PREDECESSOR_A));
    CHECK(nmc_core_set_output_lut_starts(&core, OUTPUT_Y, ROUTE_Y_TO_CORE_1, ROUTE_Y_ACK_TO_PREDECESSOR_A));
    CHECK(nmc_core_set_output_lut_starts(&core, 2, ROUTE_TERMINAL, ROUTE_TERMINAL));

    /*
     * Stage-2 ACK input LUT.  Each incoming ACK index increments one or more
     * output-group ACK counters.
     */
    CHECK(nmc_core_add_ack_output_pair_lut_entry(&core, OUTPUT_X));
    CHECK(nmc_core_add_ack_output_pair_lut_entry(&core, OUTPUT_Y));
    CHECK(nmc_core_add_ack_output_pair_lut_entry(&core, OUTPUT_X));
    CHECK(nmc_core_add_ack_output_pair_lut_entry(&core, OUTPUT_Y));

    /* Stage-1 ACK starts plus terminal: core1->[0,2), core2->[2,3), core3->[3,4). */
    CHECK(nmc_core_set_ack_lut_start(&core, ACK_FROM_CORE_1, 0));
    CHECK(nmc_core_set_ack_lut_start(&core, ACK_FROM_CORE_2, 2));
    CHECK(nmc_core_set_ack_lut_start(&core, ACK_FROM_CORE_3, 3));
    CHECK(nmc_core_set_ack_lut_start(&core, ACK_TERMINAL, 4));

    /* Stage-2 input/output pair LUT. */
    CHECK(nmc_core_add_input_output_pair_lut_entry(&core, OUTPUT_X, OUTPUT_X_INPUT_A_OFFSET));
    CHECK(nmc_core_add_input_output_pair_lut_entry(&core, OUTPUT_Y, OUTPUT_Y_INPUT_A_OFFSET));
    CHECK(nmc_core_add_input_output_pair_lut_entry(&core, OUTPUT_X, OUTPUT_X_INPUT_B_OFFSET));

    /* Stage-1 input starts plus terminal: A->[0,2), B->[2,3). */
    CHECK(nmc_core_set_input_lut_start(&core, INPUT_A, 0));
    CHECK(nmc_core_set_input_lut_start(&core, INPUT_B, 2));
    CHECK(nmc_core_set_input_lut_start(&core, 2, 3));

    /* Three natural steps with different payloads and arrival orders. */
    const NmcInputTile step0_a = {.width = INPUT_WIDTH, .group_index = INPUT_A, .payload = {0x15u}};
    const NmcInputTile step0_b = {.width = INPUT_WIDTH, .group_index = INPUT_B, .payload = {0x2Au}};
    const NmcInputTile step1_a = {.width = INPUT_WIDTH, .group_index = INPUT_A, .payload = {0x03u}};
    const NmcInputTile step1_b = {.width = INPUT_WIDTH, .group_index = INPUT_B, .payload = {0xC0u}};
    const NmcInputTile step2_b = {.width = INPUT_WIDTH, .group_index = INPUT_B, .payload = {0x18u}};
    const NmcInputTile step2_a = {.width = INPUT_WIDTH, .group_index = INPUT_A, .payload = {0x01u}};

    begin_step("initial output counters");
    print_output_counters(&core);

    begin_step("natural step 0: input A arrives, so Y fires and X waits for B");
    print_input_tile(&step0_a);
    CHECK(nmc_core_process_input_tile(&core, &step0_a));
    CHECK(core.last_input_tile_event_count == 3u);
    CHECK(core.last_input_tile_encoder_cycles == 3u);
    CHECK(core.last_input_tile_compute_cycles == 16u);
    print_compute_counters(&core);
    CHECK(drain_outputs(&core) == 1u);
    CHECK(drain_acks(&core) == 1u);
    print_output_counters(&core);

    begin_step("natural step 0: input B arrives, so X now fires");
    print_input_tile(&step0_b);
    CHECK(nmc_core_process_input_tile(&core, &step0_b));
    CHECK(core.last_input_tile_event_count == 3u);
    CHECK(core.last_input_tile_encoder_cycles == 2u);
    CHECK(core.last_input_tile_compute_cycles == 8u);
    print_compute_counters(&core);
    CHECK(drain_outputs(&core) == 1u);
    CHECK(drain_acks(&core) == 1u);
    print_output_counters(&core);

    begin_step("natural step 1: predecessor ACKs arrived, so input A can arrive before successor ACKs");
    print_input_tile(&step1_a);
    CHECK(nmc_core_process_input_tile(&core, &step1_a));
    CHECK(core.last_input_tile_event_count == 2u);
    CHECK(core.last_input_tile_encoder_cycles == 3u);
    CHECK(core.last_input_tile_compute_cycles == 16u);
    print_compute_counters(&core);
    CHECK(drain_outputs(&core) == 0u);
    CHECK(drain_acks(&core) == 0u);
    print_output_counters(&core);

    begin_step("natural step 1: input B is consumed, but X still waits for successor ACKs");
    print_input_tile(&step1_b);
    CHECK(nmc_core_process_input_tile(&core, &step1_b));
    CHECK(core.last_input_tile_event_count == 2u);
    CHECK(core.last_input_tile_encoder_cycles == 2u);
    CHECK(core.last_input_tile_compute_cycles == 8u);
    print_compute_counters(&core);
    CHECK(drain_outputs(&core) == 0u);
    CHECK(drain_acks(&core) == 0u);
    print_output_counters(&core);

    begin_step("common successor ACK from core 1 advances both X and Y, but releases neither");
    CHECK(process_successor_ack(&core, ACK_FROM_CORE_1));
    CHECK(drain_outputs(&core) == 0u);
    CHECK(drain_acks(&core) == 0u);
    print_output_counters(&core);

    begin_step("Y-only successor ACK from core 3 releases Y's pending step 1 output");
    CHECK(process_successor_ack(&core, ACK_FROM_CORE_3));
    CHECK(drain_outputs(&core) == 1u);
    CHECK(drain_acks(&core) == 1u);
    print_output_counters(&core);

    begin_step("X-only successor ACK from core 2 releases X's pending step 1 output");
    CHECK(process_successor_ack(&core, ACK_FROM_CORE_2));
    CHECK(drain_outputs(&core) == 1u);
    CHECK(drain_acks(&core) == 1u);
    print_output_counters(&core);

    begin_step("natural step 2: predecessor ACKs arrived; input B arrives first before successor ACKs");
    print_input_tile(&step2_b);
    CHECK(nmc_core_process_input_tile(&core, &step2_b));
    CHECK(core.last_input_tile_event_count == 2u);
    CHECK(core.last_input_tile_encoder_cycles == 2u);
    CHECK(core.last_input_tile_compute_cycles == 4u);
    print_compute_counters(&core);
    CHECK(drain_outputs(&core) == 0u);
    CHECK(drain_acks(&core) == 0u);
    print_output_counters(&core);

    begin_step("natural step 2: input A arrives second; both groups are complete but ACK-blocked");
    print_input_tile(&step2_a);
    CHECK(nmc_core_process_input_tile(&core, &step2_a));
    CHECK(core.last_input_tile_event_count == 1u);
    CHECK(core.last_input_tile_encoder_cycles == 2u);
    CHECK(core.last_input_tile_compute_cycles == 8u);
    print_compute_counters(&core);
    CHECK(drain_outputs(&core) == 0u);
    CHECK(drain_acks(&core) == 0u);
    print_output_counters(&core);

    begin_step("successor ACKs for step 1 flush the pending step 2 outputs");
    CHECK(process_successor_ack(&core, ACK_FROM_CORE_1));
    CHECK(drain_outputs(&core) == 0u);
    CHECK(drain_acks(&core) == 0u);
    CHECK(process_successor_ack(&core, ACK_FROM_CORE_3));
    CHECK(drain_outputs(&core) == 1u);
    CHECK(drain_acks(&core) == 1u);
    CHECK(process_successor_ack(&core, ACK_FROM_CORE_2));
    CHECK(drain_outputs(&core) == 1u);
    CHECK(drain_acks(&core) == 1u);
    print_output_counters(&core);

    begin_step("test bench passed");

    begin_step("event encoder: P-wide search replaces an empty window before a sparse event");
    enum {
        ENCODER_CORE_ID = 20,
        ENCODER_INPUT = 0,
        ENCODER_OUTPUT = 0,
        ENCODER_INPUT_WIDTH = 32,
        ENCODER_OUTPUT_WIDTH = 8,
        ENCODER_WEIGHT_COUNT = ENCODER_INPUT_WIDTH * ENCODER_OUTPUT_WIDTH,
    };
    int16_t encoder_weights[ENCODER_WEIGHT_COUNT];
    fill_weights(encoder_weights, sizeof(encoder_weights) / sizeof(encoder_weights[0]));
    const int32_t encoder_thresholds[] = {1, 1, 1, 1, 1, 1, 1, 1};
    NmcCore encoder_core;
    nmc_core_init(&encoder_core, ENCODER_CORE_ID, encoder_weights, sizeof(encoder_weights) / sizeof(encoder_weights[0]));
    CHECK(nmc_core_set_output_parallelism(&encoder_core, 4u));
    CHECK(nmc_core_set_input_parallelism(&encoder_core, 4u));
    CHECK(nmc_core_add_input_group(&encoder_core));
    CHECK(nmc_core_add_output_group(&encoder_core, ENCODER_OUTPUT_WIDTH, encoder_thresholds));
    CHECK(nmc_core_add_output_successor_lut_entry(&encoder_core, ENCODER_CORE_ID, ENCODER_INPUT));
    CHECK(nmc_core_set_output_lut_starts(&encoder_core, ENCODER_OUTPUT, 0, 1));
    CHECK(nmc_core_set_output_lut_starts(&encoder_core, 1, 1, 1));
    CHECK(nmc_core_add_input_output_pair_lut_entry(&encoder_core, ENCODER_OUTPUT, 0));
    CHECK(nmc_core_set_input_lut_start(&encoder_core, ENCODER_INPUT, 0));
    CHECK(nmc_core_set_input_lut_start(&encoder_core, 1, 1));
    const NmcInputTile sparse_32_bit_tile = {
        .width = ENCODER_INPUT_WIDTH,
        .group_index = ENCODER_INPUT,
        .payload = {0x00u, 0x00u, 0x10u, 0x00u},
    };
    print_input_tile(&sparse_32_bit_tile);
    CHECK(nmc_core_process_input_tile(&encoder_core, &sparse_32_bit_tile));
    CHECK(encoder_core.last_input_tile_event_count == 1u);
    CHECK(encoder_core.last_input_tile_encoder_cycles == 2u);
    CHECK(encoder_core.last_input_tile_compute_cycles == 2u);
    print_compute_counters(&encoder_core);
    CHECK(drain_outputs(&encoder_core) == 1u);
    CHECK(drain_acks(&encoder_core) == 0u);

    begin_step("event encoder test bench passed");

    begin_step("standalone router: XY multicast splits by next-hop direction");
    NmcRouter xy_router;
    nmc_router_init(&xy_router, CORE_ID, (NmcMeshCoordinate){.x = 1, .y = 1}, NMC_ROUTER_XY);
    const NmcRouterMessage xy_multicast = {
        .destination_count = 6u,
        .width = INPUT_WIDTH,
        .destinations = {
            {.coordinate = {.x = 1, .y = 1}, .group_index = 0},
            {.coordinate = {.x = 3, .y = 1}, .group_index = 1},
            {.coordinate = {.x = 0, .y = 1}, .group_index = 2},
            {.coordinate = {.x = 1, .y = 0}, .group_index = 3},
            {.coordinate = {.x = 1, .y = 2}, .group_index = 4},
            {.coordinate = {.x = 2, .y = 3}, .group_index = 5},
        },
        .payload = {0xA5u},
    };
    CHECK(nmc_router_route_message(&xy_router, &xy_multicast));
    CHECK(drain_router_port_expect(&xy_router, NMC_ROUTER_PORT_LOCAL, 1u, 1u));
    CHECK(drain_router_port_expect(&xy_router, NMC_ROUTER_PORT_EAST, 1u, 2u));
    CHECK(drain_router_port_expect(&xy_router, NMC_ROUTER_PORT_WEST, 1u, 1u));
    CHECK(drain_router_port_expect(&xy_router, NMC_ROUTER_PORT_NORTH, 1u, 1u));
    CHECK(drain_router_port_expect(&xy_router, NMC_ROUTER_PORT_SOUTH, 1u, 1u));

    begin_step("standalone router: YX chooses the vertical hop before the horizontal hop");
    NmcRouter yx_router;
    nmc_router_init(&yx_router, CORE_ID, (NmcMeshCoordinate){.x = 1, .y = 1}, NMC_ROUTER_YX);
    const NmcRouterMessage yx_message = {
        .destination_count = 1u,
        .width = INPUT_WIDTH,
        .destinations = {
            {.coordinate = {.x = 2, .y = 3}, .group_index = 6},
        },
        .payload = {0x3Cu},
    };
    CHECK(nmc_router_route_message(&yx_router, &yx_message));
    CHECK(drain_router_port_expect(&yx_router, NMC_ROUTER_PORT_EAST, 0u, 0u));
    CHECK(drain_router_port_expect(&yx_router, NMC_ROUTER_PORT_SOUTH, 1u, 1u));

    begin_step("router test bench passed");

    begin_step("multi-core mesh: side-injected input enters through an edge router");
    enum {
        MULTI_SOURCE_CORE = 10,
        MULTI_CONSUMER_CORE = 11,
        MULTI_OBSERVER_CORE = 12,
        MULTI_INPUT = 0,
        MULTI_ACK_FROM_CONSUMER = 0,
        MULTI_OUTPUT = 0,
        MULTI_WIDTH = 8,
        MULTI_WEIGHT_COUNT = MULTI_WIDTH * MULTI_WIDTH,
    };

    const NmcCoreMeshMapEntry mesh_map[] = {
        {.core_id = MULTI_SOURCE_CORE, .coordinate = {.x = 0, .y = 0}},
        {.core_id = MULTI_CONSUMER_CORE, .coordinate = {.x = 1, .y = 1}},
        {.core_id = MULTI_OBSERVER_CORE, .coordinate = {.x = 1, .y = 0}},
    };
    const size_t mesh_map_count = sizeof(mesh_map) / sizeof(mesh_map[0]);

    NmcRouter router_00;
    NmcRouter router_10;
    NmcRouter router_01;
    NmcRouter router_11;
    nmc_router_init(&router_00, MULTI_SOURCE_CORE, (NmcMeshCoordinate){.x = 0, .y = 0}, NMC_ROUTER_XY);
    nmc_router_init(&router_10, MULTI_OBSERVER_CORE, (NmcMeshCoordinate){.x = 1, .y = 0}, NMC_ROUTER_XY);
    nmc_router_init(&router_01, NMC_INVALID_INDEX, (NmcMeshCoordinate){.x = 0, .y = 1}, NMC_ROUTER_XY);
    nmc_router_init(&router_11, MULTI_CONSUMER_CORE, (NmcMeshCoordinate){.x = 1, .y = 1}, NMC_ROUTER_XY);

    int16_t source_weights[MULTI_WEIGHT_COUNT];
    int16_t consumer_weights[MULTI_WEIGHT_COUNT];
    int16_t observer_weights[MULTI_WEIGHT_COUNT];
    fill_weights(source_weights, sizeof(source_weights) / sizeof(source_weights[0]));
    fill_weights(consumer_weights, sizeof(consumer_weights) / sizeof(consumer_weights[0]));
    fill_weights(observer_weights, sizeof(observer_weights) / sizeof(observer_weights[0]));

    const int32_t multi_thresholds[] = {1, 1, 1, 1, 1, 1, 1, 1};

    NmcCore source_core;
    nmc_core_init(&source_core, MULTI_SOURCE_CORE, source_weights, sizeof(source_weights) / sizeof(source_weights[0]));
    CHECK(nmc_core_set_output_parallelism(&source_core, 4u));
    CHECK(nmc_core_set_input_parallelism(&source_core, 4u));
    CHECK(nmc_core_add_input_group(&source_core));
    CHECK(nmc_core_add_ack_group(&source_core));
    CHECK(nmc_core_add_output_group(&source_core, MULTI_WIDTH, multi_thresholds));
    CHECK(nmc_core_add_output_successor_lut_entry(&source_core, MULTI_CONSUMER_CORE, MULTI_INPUT));
    CHECK(nmc_core_set_output_lut_starts(&source_core, MULTI_OUTPUT, 0, 1));
    CHECK(nmc_core_set_output_lut_starts(&source_core, 1, 1, 1));
    CHECK(nmc_core_add_ack_output_pair_lut_entry(&source_core, MULTI_OUTPUT));
    CHECK(nmc_core_set_ack_lut_start(&source_core, MULTI_ACK_FROM_CONSUMER, 0));
    CHECK(nmc_core_set_ack_lut_start(&source_core, 1, 1));
    CHECK(nmc_core_add_input_output_pair_lut_entry(&source_core, MULTI_OUTPUT, 0));
    CHECK(nmc_core_set_input_lut_start(&source_core, MULTI_INPUT, 0));
    CHECK(nmc_core_set_input_lut_start(&source_core, 1, 1));

    NmcCore consumer_core;
    nmc_core_init(&consumer_core, MULTI_CONSUMER_CORE, consumer_weights, sizeof(consumer_weights) / sizeof(consumer_weights[0]));
    CHECK(nmc_core_set_output_parallelism(&consumer_core, 4u));
    CHECK(nmc_core_set_input_parallelism(&consumer_core, 4u));
    CHECK(nmc_core_add_input_group(&consumer_core));
    CHECK(nmc_core_add_output_group(&consumer_core, MULTI_WIDTH, multi_thresholds));
    CHECK(nmc_core_add_output_successor_lut_entry(&consumer_core, MULTI_OBSERVER_CORE, MULTI_INPUT));
    CHECK(nmc_core_add_output_predecessor_lut_entry(&consumer_core, MULTI_SOURCE_CORE, MULTI_ACK_FROM_CONSUMER));
    CHECK(nmc_core_set_output_lut_starts(&consumer_core, MULTI_OUTPUT, 0, 1));
    CHECK(nmc_core_set_output_lut_starts(&consumer_core, 1, 2, 2));
    CHECK(nmc_core_add_input_output_pair_lut_entry(&consumer_core, MULTI_OUTPUT, 0));
    CHECK(nmc_core_set_input_lut_start(&consumer_core, MULTI_INPUT, 0));
    CHECK(nmc_core_set_input_lut_start(&consumer_core, 1, 1));

    NmcCore observer_core;
    nmc_core_init(&observer_core, MULTI_OBSERVER_CORE, observer_weights, sizeof(observer_weights) / sizeof(observer_weights[0]));
    CHECK(nmc_core_set_output_parallelism(&observer_core, 4u));
    CHECK(nmc_core_set_input_parallelism(&observer_core, 4u));
    CHECK(nmc_core_add_input_group(&observer_core));
    CHECK(nmc_core_add_output_group(&observer_core, MULTI_WIDTH, multi_thresholds));
    CHECK(nmc_core_set_output_lut_starts(&observer_core, MULTI_OUTPUT, 0, 0));
    CHECK(nmc_core_set_output_lut_starts(&observer_core, 1, 0, 0));
    CHECK(nmc_core_add_input_output_pair_lut_entry(&observer_core, MULTI_OUTPUT, 0));
    CHECK(nmc_core_set_input_lut_start(&observer_core, MULTI_INPUT, 0));
    CHECK(nmc_core_set_input_lut_start(&observer_core, 1, 1));

    const NmcRouterMessage side_input_step0 = {
        .destination_count = 1u,
        .width = MULTI_WIDTH,
        .destinations = {
            {.coordinate = {.x = 0, .y = 0}, .group_index = MULTI_INPUT},
        },
        .payload = {0x0Fu},
    };

    puts("west-side injector pushes a tile into edge router (0,0)");
    CHECK(nmc_router_route_message(&router_00, &side_input_step0));
    CHECK(deliver_router_local_to_core_expect(&router_00, &source_core, 1u, 0u));
    CHECK(source_core.last_input_tile_event_count == 4u);
    CHECK(source_core.last_input_tile_encoder_cycles == 2u);
    CHECK(source_core.last_input_tile_compute_cycles == 4u);
    CHECK(inject_core_outputs_into_router_expect(&source_core, &router_00, mesh_map, mesh_map_count, 1u));
    CHECK(inject_core_acks_into_router_expect(&source_core, &router_00, mesh_map, mesh_map_count, 0u));

    begin_step("multi-core mesh: source tile walks east then south to the consumer core");
    CHECK(transfer_router_port_expect(&router_00, NMC_ROUTER_PORT_EAST, &router_10, 1u, 1u));
    CHECK(transfer_router_port_expect(&router_10, NMC_ROUTER_PORT_SOUTH, &router_11, 1u, 1u));
    CHECK(deliver_router_local_to_core_expect(&router_11, &consumer_core, 1u, 0u));
    CHECK(consumer_core.last_input_tile_event_count == 8u);
    CHECK(consumer_core.last_input_tile_encoder_cycles == 2u);
    CHECK(consumer_core.last_input_tile_compute_cycles == 4u);

    begin_step("multi-core mesh: consumer emits to observer and ACKs the source");
    CHECK(inject_core_outputs_into_router_expect(&consumer_core, &router_11, mesh_map, mesh_map_count, 1u));
    CHECK(inject_core_acks_into_router_expect(&consumer_core, &router_11, mesh_map, mesh_map_count, 1u));

    CHECK(transfer_router_port_expect(&router_11, NMC_ROUTER_PORT_NORTH, &router_10, 1u, 1u));
    CHECK(deliver_router_local_to_core_expect(&router_10, &observer_core, 1u, 0u));
    CHECK(observer_core.last_input_tile_event_count == 8u);
    CHECK(observer_core.last_input_tile_encoder_cycles == 2u);
    CHECK(observer_core.last_input_tile_compute_cycles == 4u);
    CHECK(observer_core.output_groups[MULTI_OUTPUT].input_count == 1u);

    CHECK(transfer_router_port_expect(&router_11, NMC_ROUTER_PORT_WEST, &router_01, 1u, 1u));
    CHECK(transfer_router_port_expect(&router_01, NMC_ROUTER_PORT_NORTH, &router_00, 1u, 1u));
    CHECK(deliver_router_local_to_core_expect(&router_00, &source_core, 0u, 1u));
    CHECK(source_core.output_groups[MULTI_OUTPUT].ack_count == 1u);

    begin_step("multi-core mesh: returned ACK lets the edge injector send the next input");
    const NmcRouterMessage side_input_step1 = {
        .destination_count = 1u,
        .width = MULTI_WIDTH,
        .destinations = {
            {.coordinate = {.x = 0, .y = 0}, .group_index = MULTI_INPUT},
        },
        .payload = {0xF0u},
    };
    puts("west-side injector pushes a second tile into edge router (0,0)");
    CHECK(nmc_router_route_message(&router_00, &side_input_step1));
    CHECK(deliver_router_local_to_core_expect(&router_00, &source_core, 1u, 0u));
    CHECK(source_core.last_input_tile_event_count == 4u);
    CHECK(source_core.last_input_tile_encoder_cycles == 2u);
    CHECK(source_core.last_input_tile_compute_cycles == 4u);
    CHECK(inject_core_outputs_into_router_expect(&source_core, &router_00, mesh_map, mesh_map_count, 1u));

    begin_step("multi-core mesh test bench passed");
    return EXIT_SUCCESS;
}
