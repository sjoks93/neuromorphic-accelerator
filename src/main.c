#include "nmc/core.h"
#include "nmc/router.h"

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

#define ARRAY_COUNT(array) (sizeof(array) / sizeof((array)[0]))

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
           NMC_OUTPUT_PARALLELISM,
           NMC_INPUT_PARALLELISM,
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
        OUTPUT_X = 0,
        OUTPUT_Y = 1,
        ACK_FROM_CORE_1 = 0,
        ACK_FROM_CORE_2 = 1,
        ACK_FROM_CORE_3 = 2,
        INPUT_WIDTH = 8,
        OUTPUT_X_WIDTH = 8,
        OUTPUT_Y_WIDTH = 8,
    };

    /*
     * Weight matrices are auto-placed by the mapper.  All weights are one, so
     * each output neuron sees the input popcount.
     */
    /* X needs both inputs; Y needs only A. */
    const int32_t thresholds_x[] = {2, 2, 2, 2, 2, 2, 2, 2};
    const int32_t thresholds_y[] = {1, 1, 1, 1, 1, 1, 1, 1};
    int16_t weights_8x8[OUTPUT_X_WIDTH * INPUT_WIDTH];
    fill_weights(weights_8x8, ARRAY_COUNT(weights_8x8));
    const NmcInputGroupMappingSpec input_groups[] = {
        [INPUT_A] = NMC_INPUT_GROUP(INPUT_WIDTH),
        [INPUT_B] = NMC_INPUT_GROUP(INPUT_WIDTH),
    };
    const NmcInputConnectionMappingSpec output_x_inputs[] = {
        NMC_INPUT_CONNECTION(INPUT_A, weights_8x8),
        NMC_INPUT_CONNECTION(INPUT_B, weights_8x8),
    };
    const NmcOutputSuccessorMappingSpec output_x_successors[] = {
        NMC_SUCCESSOR(1, 0),
        NMC_SUCCESSOR(2, 0),
    };
    const NmcPredecessorMappingSpec output_x_predecessors[] = {
        NMC_PREDECESSOR(CORE_ID, ACK_FROM_CORE_1),
        NMC_PREDECESSOR(CORE_ID, ACK_FROM_CORE_2),
    };
    const NmcInputConnectionMappingSpec output_y_inputs[] = {
        NMC_INPUT_CONNECTION(INPUT_A, weights_8x8),
    };
    const NmcOutputSuccessorMappingSpec output_y_successors[] = {
        NMC_SUCCESSOR(1, 0),
        NMC_SUCCESSOR(3, 1),
    };
    const NmcPredecessorMappingSpec output_y_predecessors[] = {
        NMC_PREDECESSOR(CORE_ID, ACK_FROM_CORE_1),
    };
    const NmcOutputGroupMappingSpec output_groups[] = {
        {
            .width = OUTPUT_X_WIDTH,
            .thresholds = thresholds_x,
            .accumulator_start = NMC_AUTO_ACCUMULATOR_OFFSET,
            .inputs = output_x_inputs,
            .input_count = ARRAY_COUNT(output_x_inputs),
            .successors = output_x_successors,
            .successor_count = ARRAY_COUNT(output_x_successors),
            .predecessors = output_x_predecessors,
            .predecessor_count = ARRAY_COUNT(output_x_predecessors),
        },
        {
            .width = OUTPUT_Y_WIDTH,
            .thresholds = thresholds_y,
            .accumulator_start = NMC_AUTO_ACCUMULATOR_OFFSET,
            .inputs = output_y_inputs,
            .input_count = ARRAY_COUNT(output_y_inputs),
            .successors = output_y_successors,
            .successor_count = ARRAY_COUNT(output_y_successors),
            .predecessors = output_y_predecessors,
            .predecessor_count = ARRAY_COUNT(output_y_predecessors),
        },
    };
    const NmcCoreMappingSpec mapping = {
        .input_groups = input_groups,
        .input_group_count = ARRAY_COUNT(input_groups),
        .output_groups = output_groups,
        .output_group_count = ARRAY_COUNT(output_groups),
    };

    nmc_ack_index_t ack_from_core_1 = NMC_INVALID_INDEX;
    nmc_ack_index_t ack_from_core_2 = NMC_INVALID_INDEX;
    nmc_ack_index_t ack_from_core_3 = NMC_INVALID_INDEX;
    CHECK(nmc_core_mapping_ack_group_for_successor(&mapping, 1, 0, &ack_from_core_1));
    CHECK(nmc_core_mapping_ack_group_for_successor(&mapping, 2, 0, &ack_from_core_2));
    CHECK(nmc_core_mapping_ack_group_for_successor(&mapping, 3, 1, &ack_from_core_3));

    /* Create one core under test. */
    NmcCore core;
    CHECK(nmc_core_configure_mapping(&core, CORE_ID, &mapping));

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
    CHECK(core.last_input_tile_encoder_cycles == 2u);
    CHECK(core.last_input_tile_compute_cycles == 4u);
    print_compute_counters(&core);
    CHECK(drain_outputs(&core) == 1u);
    CHECK(drain_acks(&core) == 1u);
    print_output_counters(&core);

    begin_step("natural step 0: input B arrives, so X now fires");
    print_input_tile(&step0_b);
    CHECK(nmc_core_process_input_tile(&core, &step0_b));
    CHECK(core.last_input_tile_event_count == 3u);
    CHECK(core.last_input_tile_encoder_cycles == 1u);
    CHECK(core.last_input_tile_compute_cycles == 2u);
    print_compute_counters(&core);
    CHECK(drain_outputs(&core) == 1u);
    CHECK(drain_acks(&core) == 1u);
    print_output_counters(&core);

    begin_step("natural step 1: predecessor ACKs arrived, so input A can arrive before successor ACKs");
    print_input_tile(&step1_a);
    CHECK(nmc_core_process_input_tile(&core, &step1_a));
    CHECK(core.last_input_tile_event_count == 2u);
    CHECK(core.last_input_tile_encoder_cycles == 2u);
    CHECK(core.last_input_tile_compute_cycles == 8u);
    print_compute_counters(&core);
    CHECK(drain_outputs(&core) == 0u);
    CHECK(drain_acks(&core) == 0u);
    print_output_counters(&core);

    begin_step("natural step 1: input B is consumed, but X still waits for successor ACKs");
    print_input_tile(&step1_b);
    CHECK(nmc_core_process_input_tile(&core, &step1_b));
    CHECK(core.last_input_tile_event_count == 2u);
    CHECK(core.last_input_tile_encoder_cycles == 2u);
    CHECK(core.last_input_tile_compute_cycles == 4u);
    print_compute_counters(&core);
    CHECK(drain_outputs(&core) == 0u);
    CHECK(drain_acks(&core) == 0u);
    print_output_counters(&core);

    begin_step("common successor ACK from core 1 advances both X and Y, but releases neither");
    CHECK(process_successor_ack(&core, ack_from_core_1));
    CHECK(drain_outputs(&core) == 0u);
    CHECK(drain_acks(&core) == 0u);
    print_output_counters(&core);

    begin_step("Y-only successor ACK from core 3 releases Y's pending step 1 output");
    CHECK(process_successor_ack(&core, ack_from_core_3));
    CHECK(drain_outputs(&core) == 1u);
    CHECK(drain_acks(&core) == 1u);
    print_output_counters(&core);

    begin_step("X-only successor ACK from core 2 releases X's pending step 1 output");
    CHECK(process_successor_ack(&core, ack_from_core_2));
    CHECK(drain_outputs(&core) == 1u);
    CHECK(drain_acks(&core) == 1u);
    print_output_counters(&core);

    begin_step("natural step 2: predecessor ACKs arrived; input B arrives first before successor ACKs");
    print_input_tile(&step2_b);
    CHECK(nmc_core_process_input_tile(&core, &step2_b));
    CHECK(core.last_input_tile_event_count == 2u);
    CHECK(core.last_input_tile_encoder_cycles == 2u);
    CHECK(core.last_input_tile_compute_cycles == 2u);
    print_compute_counters(&core);
    CHECK(drain_outputs(&core) == 0u);
    CHECK(drain_acks(&core) == 0u);
    print_output_counters(&core);

    begin_step("natural step 2: input A arrives second; both groups are complete but ACK-blocked");
    print_input_tile(&step2_a);
    CHECK(nmc_core_process_input_tile(&core, &step2_a));
    CHECK(core.last_input_tile_event_count == 1u);
    CHECK(core.last_input_tile_encoder_cycles == 2u);
    CHECK(core.last_input_tile_compute_cycles == 4u);
    print_compute_counters(&core);
    CHECK(drain_outputs(&core) == 0u);
    CHECK(drain_acks(&core) == 0u);
    print_output_counters(&core);

    begin_step("successor ACKs for step 1 flush the pending step 2 outputs");
    CHECK(process_successor_ack(&core, ack_from_core_1));
    CHECK(drain_outputs(&core) == 0u);
    CHECK(drain_acks(&core) == 0u);
    CHECK(process_successor_ack(&core, ack_from_core_3));
    CHECK(drain_outputs(&core) == 1u);
    CHECK(drain_acks(&core) == 1u);
    CHECK(process_successor_ack(&core, ack_from_core_2));
    CHECK(drain_outputs(&core) == 1u);
    CHECK(drain_acks(&core) == 1u);
    print_output_counters(&core);

    begin_step("test bench passed");

    begin_step("event encoder: P-wide search replaces an empty window before a sparse event");
    enum {
        ENCODER_CORE_ID = 20,
        ENCODER_OUTPUT = 0,
        ENCODER_INPUT = 0,
        ENCODER_INPUT_WIDTH = 32,
        ENCODER_OUTPUT_WIDTH = 8,
    };
    const int32_t encoder_thresholds[] = {1, 1, 1, 1, 1, 1, 1, 1};
    int16_t encoder_weights[ENCODER_OUTPUT_WIDTH * ENCODER_INPUT_WIDTH];
    fill_weights(encoder_weights, ARRAY_COUNT(encoder_weights));
    NmcCore encoder_core;
    const NmcInputGroupMappingSpec encoder_inputs[] = {
        [ENCODER_INPUT] = NMC_INPUT_GROUP(ENCODER_INPUT_WIDTH),
    };
    const NmcInputConnectionMappingSpec encoder_output_inputs[] = {
        NMC_INPUT_CONNECTION(ENCODER_INPUT, encoder_weights),
    };
    const NmcOutputSuccessorMappingSpec encoder_successors[] = {
        NMC_SUCCESSOR(ENCODER_CORE_ID, ENCODER_INPUT),
    };
    const NmcOutputGroupMappingSpec encoder_outputs[] = {
        {
            .width = ENCODER_OUTPUT_WIDTH,
            .thresholds = encoder_thresholds,
            .accumulator_start = NMC_AUTO_ACCUMULATOR_OFFSET,
            .inputs = encoder_output_inputs,
            .input_count = ARRAY_COUNT(encoder_output_inputs),
            .successors = encoder_successors,
            .successor_count = ARRAY_COUNT(encoder_successors),
        },
    };
    const NmcCoreMappingSpec encoder_mapping = {
        .input_groups = encoder_inputs,
        .input_group_count = ARRAY_COUNT(encoder_inputs),
        .output_groups = encoder_outputs,
        .output_group_count = ARRAY_COUNT(encoder_outputs),
    };
    CHECK(nmc_core_configure_mapping(&encoder_core, ENCODER_CORE_ID, &encoder_mapping));
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

    begin_step("graph mapper: generate core-local LUT indices from logical groups");
    enum {
        MAPPER_EXTERNAL_IMAGE = 0,
        MAPPER_EXTERNAL_BIAS = 1,
        MAPPER_SOURCE_GROUP = 100,
        MAPPER_MIDDLE_GROUP = 101,
        MAPPER_OBSERVER_GROUP = 102,
        MAPPER_SIDE_GROUP = 103,
        MAPPER_SOURCE_CORE = 30,
        MAPPER_MIDDLE_CORE = 31,
        MAPPER_OBSERVER_CORE = 32,
        MAPPER_WIDTH = 8,
    };
    const int32_t mapper_thresholds[] = {1, 1, 1, 1, 1, 1, 1, 1};
    int16_t mapper_external_weights[MAPPER_WIDTH * MAPPER_WIDTH];
    int16_t mapper_external_side_weights[MAPPER_WIDTH * MAPPER_WIDTH];
    int16_t mapper_external_observer_weights[MAPPER_WIDTH * MAPPER_WIDTH];
    int16_t mapper_source_middle_weights[MAPPER_WIDTH * MAPPER_WIDTH];
    int16_t mapper_source_observer_weights[MAPPER_WIDTH * MAPPER_WIDTH];
    int16_t mapper_middle_observer_weights[MAPPER_WIDTH * MAPPER_WIDTH];
    fill_weights(mapper_external_weights, ARRAY_COUNT(mapper_external_weights));
    fill_weights(mapper_external_side_weights, ARRAY_COUNT(mapper_external_side_weights));
    fill_weights(mapper_external_observer_weights, ARRAY_COUNT(mapper_external_observer_weights));
    fill_weights(mapper_source_middle_weights, ARRAY_COUNT(mapper_source_middle_weights));
    fill_weights(mapper_source_observer_weights, ARRAY_COUNT(mapper_source_observer_weights));
    fill_weights(mapper_middle_observer_weights, ARRAY_COUNT(mapper_middle_observer_weights));

    const NmcNetworkGroupMappingSpec mapper_groups[] = {
        {.group_id = MAPPER_SOURCE_GROUP, .core_id = MAPPER_SOURCE_CORE, .width = MAPPER_WIDTH, .thresholds = mapper_thresholds},
        {.group_id = MAPPER_SIDE_GROUP, .core_id = MAPPER_SOURCE_CORE, .width = MAPPER_WIDTH, .thresholds = mapper_thresholds},
        {.group_id = MAPPER_MIDDLE_GROUP, .core_id = MAPPER_MIDDLE_CORE, .width = MAPPER_WIDTH, .thresholds = mapper_thresholds},
        {.group_id = MAPPER_OBSERVER_GROUP, .core_id = MAPPER_OBSERVER_CORE, .width = MAPPER_WIDTH, .thresholds = mapper_thresholds},
    };
    const NmcNetworkInputMappingSpec mapper_inputs[] = {
        {.input_id = MAPPER_EXTERNAL_IMAGE, .destination_group = MAPPER_SOURCE_GROUP, .width = MAPPER_WIDTH, .weights = mapper_external_weights},
        {.input_id = MAPPER_EXTERNAL_IMAGE, .destination_group = MAPPER_SIDE_GROUP, .width = MAPPER_WIDTH, .weights = mapper_external_side_weights},
        {.input_id = MAPPER_EXTERNAL_BIAS, .destination_group = MAPPER_OBSERVER_GROUP, .width = MAPPER_WIDTH, .weights = mapper_external_observer_weights},
    };
    const NmcNetworkConnectionMappingSpec mapper_connections[] = {
        {.source_group = MAPPER_SOURCE_GROUP, .destination_group = MAPPER_MIDDLE_GROUP, .weights = mapper_source_middle_weights},
        {.source_group = MAPPER_SOURCE_GROUP, .destination_group = MAPPER_OBSERVER_GROUP, .weights = mapper_source_observer_weights},
        {.source_group = MAPPER_MIDDLE_GROUP, .destination_group = MAPPER_OBSERVER_GROUP, .weights = mapper_middle_observer_weights},
    };
    const NmcNetworkMappingSpec mapper_network = {
        .groups = mapper_groups,
        .group_count = ARRAY_COUNT(mapper_groups),
        .inputs = mapper_inputs,
        .input_count = ARRAY_COUNT(mapper_inputs),
        .connections = mapper_connections,
        .connection_count = ARRAY_COUNT(mapper_connections),
    };
    NmcGeneratedMappings generated_mappings;
    CHECK(nmc_generate_core_mappings(&mapper_network, &generated_mappings));
    const NmcGeneratedCoreMapping *generated_source = nmc_generated_mappings_find_core(&generated_mappings, MAPPER_SOURCE_CORE);
    const NmcGeneratedCoreMapping *generated_middle = nmc_generated_mappings_find_core(&generated_mappings, MAPPER_MIDDLE_CORE);
    const NmcGeneratedCoreMapping *generated_observer = nmc_generated_mappings_find_core(&generated_mappings, MAPPER_OBSERVER_CORE);
    CHECK(generated_source != NULL);
    CHECK(generated_middle != NULL);
    CHECK(generated_observer != NULL);
    CHECK(generated_source->mapping.input_group_count == 1u);
    CHECK(generated_source->mapping.output_group_count == 2u);
    CHECK(generated_source->input_is_external[0]);
    CHECK(generated_source->input_external_ids[0] == MAPPER_EXTERNAL_IMAGE);
    CHECK(generated_source->mapping.output_groups[0].successor_count == 2u);
    CHECK(generated_source->mapping.output_groups[0].successors[0].core_id == MAPPER_MIDDLE_CORE);
    CHECK(generated_source->mapping.output_groups[0].successors[0].input_group == 0u);
    CHECK(generated_source->mapping.output_groups[0].successors[1].core_id == MAPPER_OBSERVER_CORE);
    CHECK(generated_source->mapping.output_groups[0].successors[1].input_group == 1u);
    CHECK(generated_source->mapping.output_groups[1].input_count == 1u);
    CHECK(generated_source->mapping.output_groups[1].inputs[0].input_group == 0u);
    CHECK(generated_middle->mapping.input_group_count == 1u);
    CHECK(generated_observer->mapping.input_group_count == 3u);
    CHECK(generated_middle->mapping.output_groups[0].predecessor_count == 1u);
    CHECK(generated_middle->mapping.output_groups[0].predecessors[0].core_id == MAPPER_SOURCE_CORE);
    CHECK(generated_middle->mapping.output_groups[0].predecessors[0].ack_group == 0u);

    NmcCore mapper_source_core;
    NmcCore mapper_middle_core;
    NmcCore mapper_observer_core;
    CHECK(nmc_core_configure_mapping(&mapper_source_core, generated_source->core_id, &generated_source->mapping));
    CHECK(nmc_core_configure_mapping(&mapper_middle_core, generated_middle->core_id, &generated_middle->mapping));
    CHECK(nmc_core_configure_mapping(&mapper_observer_core, generated_observer->core_id, &generated_observer->mapping));

    const NmcInputTile mapper_input = {.width = MAPPER_WIDTH, .group_index = 0u, .payload = {0x03u}};
    NmcNetworkTile mapper_tile;
    NmcInputTile mapper_middle_input;
    NmcAckMessage mapper_ack;
    print_input_tile(&mapper_input);
    CHECK(nmc_core_process_input_tile(&mapper_source_core, &mapper_input));
    CHECK(nmc_core_pop_output_tile(&mapper_source_core, &mapper_tile));
    CHECK(mapper_tile.destination_count == 2u);
    size_t mapper_middle_destination = NMC_INVALID_INDEX;
    bool mapper_found_observer_destination = false;
    for (size_t i = 0u; i < mapper_tile.destination_count; ++i) {
        if (mapper_tile.destinations[i].core_id == MAPPER_MIDDLE_CORE && mapper_tile.destinations[i].group_index == 0u) {
            mapper_middle_destination = i;
        }
        if (mapper_tile.destinations[i].core_id == MAPPER_OBSERVER_CORE && mapper_tile.destinations[i].group_index == 1u) {
            mapper_found_observer_destination = true;
        }
    }
    CHECK(mapper_middle_destination != NMC_INVALID_INDEX);
    CHECK(mapper_found_observer_destination);
    CHECK(nmc_network_tile_get_input_tile(&mapper_tile, mapper_middle_destination, &mapper_middle_input));
    CHECK(nmc_core_process_input_tile(&mapper_middle_core, &mapper_middle_input));
    CHECK(nmc_core_pop_output_tile(&mapper_middle_core, &mapper_tile));
    CHECK(mapper_tile.destination_count == 1u);
    CHECK(mapper_tile.destinations[0].core_id == MAPPER_OBSERVER_CORE);
    CHECK(mapper_tile.destinations[0].group_index == 2u);
    CHECK(nmc_core_pop_ack(&mapper_middle_core, &mapper_ack));
    CHECK(mapper_ack.destination_count == 1u);
    CHECK(mapper_ack.destinations[0].core_id == MAPPER_SOURCE_CORE);
    CHECK(mapper_ack.destinations[0].group_index == 0u);
    CHECK(nmc_core_process_ack(&mapper_source_core, &mapper_ack));

    begin_step("graph mapper test bench passed");

    begin_step("recurrent mapper: cyclic group connections use delayed feedback credits");
    enum {
        RECURRENT_INPUT_A = 10,
        RECURRENT_INPUT_B = 11,
        RECURRENT_GROUP_A = 200,
        RECURRENT_GROUP_B = 201,
        RECURRENT_CORE_A = 40,
        RECURRENT_CORE_B = 41,
        RECURRENT_WIDTH = 8,
    };
    const int32_t recurrent_thresholds[] = {1, 1, 1, 1, 1, 1, 1, 1};
    int16_t recurrent_external_weights[RECURRENT_WIDTH * RECURRENT_WIDTH];
    int16_t recurrent_feedback_weights[RECURRENT_WIDTH * RECURRENT_WIDTH];
    fill_weights(recurrent_external_weights, ARRAY_COUNT(recurrent_external_weights));
    fill_weights(recurrent_feedback_weights, ARRAY_COUNT(recurrent_feedback_weights));

    const NmcNetworkGroupMappingSpec recurrent_groups[] = {
        {.group_id = RECURRENT_GROUP_A, .core_id = RECURRENT_CORE_A, .width = RECURRENT_WIDTH, .thresholds = recurrent_thresholds},
        {.group_id = RECURRENT_GROUP_B, .core_id = RECURRENT_CORE_B, .width = RECURRENT_WIDTH, .thresholds = recurrent_thresholds},
    };
    const NmcNetworkInputMappingSpec recurrent_inputs[] = {
        {.input_id = RECURRENT_INPUT_A, .destination_group = RECURRENT_GROUP_A, .width = RECURRENT_WIDTH, .weights = recurrent_external_weights},
        {.input_id = RECURRENT_INPUT_B, .destination_group = RECURRENT_GROUP_B, .width = RECURRENT_WIDTH, .weights = recurrent_external_weights},
    };
    const NmcNetworkConnectionMappingSpec recurrent_connections[] = {
        {.source_group = RECURRENT_GROUP_A, .destination_group = RECURRENT_GROUP_B, .weights = recurrent_feedback_weights},
        {.source_group = RECURRENT_GROUP_B, .destination_group = RECURRENT_GROUP_A, .weights = recurrent_feedback_weights},
    };
    const NmcNetworkMappingSpec recurrent_network = {
        .groups = recurrent_groups,
        .group_count = ARRAY_COUNT(recurrent_groups),
        .inputs = recurrent_inputs,
        .input_count = ARRAY_COUNT(recurrent_inputs),
        .connections = recurrent_connections,
        .connection_count = ARRAY_COUNT(recurrent_connections),
    };
    NmcGeneratedMappings recurrent_generated;
    CHECK(nmc_generate_core_mappings(&recurrent_network, &recurrent_generated));
    const NmcGeneratedCoreMapping *generated_recurrent_a = nmc_generated_mappings_find_core(&recurrent_generated, RECURRENT_CORE_A);
    const NmcGeneratedCoreMapping *generated_recurrent_b = nmc_generated_mappings_find_core(&recurrent_generated, RECURRENT_CORE_B);
    CHECK(generated_recurrent_a != NULL);
    CHECK(generated_recurrent_b != NULL);
    CHECK(generated_recurrent_a->mapping.output_groups[0].input_count == 2u);
    CHECK(generated_recurrent_b->mapping.output_groups[0].input_count == 2u);
    CHECK(generated_recurrent_a->mapping.output_groups[0].inputs[1].recurrent);
    CHECK(generated_recurrent_b->mapping.output_groups[0].inputs[1].recurrent);
    CHECK(generated_recurrent_a->mapping.output_groups[0].predecessors[0].recurrent);
    CHECK(generated_recurrent_b->mapping.output_groups[0].predecessors[0].recurrent);

    NmcCore recurrent_core_a;
    NmcCore recurrent_core_b;
    CHECK(nmc_core_configure_mapping(&recurrent_core_a, generated_recurrent_a->core_id, &generated_recurrent_a->mapping));
    CHECK(nmc_core_configure_mapping(&recurrent_core_b, generated_recurrent_b->core_id, &generated_recurrent_b->mapping));
    CHECK(recurrent_core_a.output_groups[0].input_count == 1u);
    CHECK(recurrent_core_b.output_groups[0].input_count == 1u);
    CHECK(recurrent_core_a.output_groups[0].primed_recurrent_input_count == 1u);
    CHECK(recurrent_core_b.output_groups[0].primed_recurrent_input_count == 1u);

    const NmcInputTile recurrent_external_a_step0 = {.width = RECURRENT_WIDTH, .group_index = 0u, .payload = {0x01u}};
    const NmcInputTile recurrent_external_b_step0 = {.width = RECURRENT_WIDTH, .group_index = 0u, .payload = {0x02u}};
    CHECK(nmc_core_process_input_tile(&recurrent_core_a, &recurrent_external_a_step0));
    CHECK(nmc_core_process_input_tile(&recurrent_core_b, &recurrent_external_b_step0));

    NmcNetworkTile recurrent_tile_a_to_b;
    NmcNetworkTile recurrent_tile_b_to_a;
    CHECK(nmc_core_pop_output_tile(&recurrent_core_a, &recurrent_tile_a_to_b));
    CHECK(nmc_core_pop_output_tile(&recurrent_core_b, &recurrent_tile_b_to_a));
    CHECK(recurrent_tile_a_to_b.destinations[0].core_id == RECURRENT_CORE_B);
    CHECK(recurrent_tile_b_to_a.destinations[0].core_id == RECURRENT_CORE_A);
    CHECK(drain_acks(&recurrent_core_a) == 0u);
    CHECK(drain_acks(&recurrent_core_b) == 0u);

    NmcInputTile recurrent_input_from_a;
    NmcInputTile recurrent_input_from_b;
    CHECK(nmc_network_tile_get_input_tile(&recurrent_tile_a_to_b, 0u, &recurrent_input_from_a));
    CHECK(nmc_network_tile_get_input_tile(&recurrent_tile_b_to_a, 0u, &recurrent_input_from_b));
    CHECK(nmc_core_process_input_tile(&recurrent_core_b, &recurrent_input_from_a));
    CHECK(nmc_core_process_input_tile(&recurrent_core_a, &recurrent_input_from_b));

    const NmcInputTile recurrent_external_a_step1 = {.width = RECURRENT_WIDTH, .group_index = 0u, .payload = {0x04u}};
    const NmcInputTile recurrent_external_b_step1 = {.width = RECURRENT_WIDTH, .group_index = 0u, .payload = {0x08u}};
    CHECK(nmc_core_process_input_tile(&recurrent_core_a, &recurrent_external_a_step1));
    CHECK(nmc_core_process_input_tile(&recurrent_core_b, &recurrent_external_b_step1));
    CHECK(!nmc_core_pop_output_tile(&recurrent_core_a, &recurrent_tile_b_to_a));
    CHECK(!nmc_core_pop_output_tile(&recurrent_core_b, &recurrent_tile_a_to_b));

    NmcAckMessage recurrent_ack_a_to_b;
    NmcAckMessage recurrent_ack_b_to_a;
    CHECK(nmc_core_pop_ack(&recurrent_core_a, &recurrent_ack_a_to_b));
    CHECK(nmc_core_pop_ack(&recurrent_core_b, &recurrent_ack_b_to_a));
    CHECK(recurrent_ack_a_to_b.destinations[0].core_id == RECURRENT_CORE_B);
    CHECK(recurrent_ack_b_to_a.destinations[0].core_id == RECURRENT_CORE_A);
    CHECK(nmc_core_process_ack(&recurrent_core_a, &recurrent_ack_b_to_a));
    CHECK(nmc_core_process_ack(&recurrent_core_b, &recurrent_ack_a_to_b));
    CHECK(nmc_core_pop_output_tile(&recurrent_core_a, &recurrent_tile_a_to_b));
    CHECK(nmc_core_pop_output_tile(&recurrent_core_b, &recurrent_tile_b_to_a));

    begin_step("recurrent mapper test bench passed");

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
        MULTI_OUTPUT = 0,
        MULTI_INPUT = 0,
        MULTI_ACK_FROM_FIRST_SUCCESSOR = 0,
        MULTI_WIDTH = 8,
    };

    const NmcCoreMeshMapEntry mesh_map[] = {
        {.core_id = MULTI_SOURCE_CORE, .coordinate = {.x = 0, .y = 0}},
        {.core_id = MULTI_CONSUMER_CORE, .coordinate = {.x = 1, .y = 1}},
        {.core_id = MULTI_OBSERVER_CORE, .coordinate = {.x = 1, .y = 0}},
    };
    const size_t mesh_map_count = ARRAY_COUNT(mesh_map);

    NmcRouter router_00;
    NmcRouter router_10;
    NmcRouter router_01;
    NmcRouter router_11;
    nmc_router_init(&router_00, MULTI_SOURCE_CORE, (NmcMeshCoordinate){.x = 0, .y = 0}, NMC_ROUTER_XY);
    nmc_router_init(&router_10, MULTI_OBSERVER_CORE, (NmcMeshCoordinate){.x = 1, .y = 0}, NMC_ROUTER_XY);
    nmc_router_init(&router_01, NMC_INVALID_INDEX, (NmcMeshCoordinate){.x = 0, .y = 1}, NMC_ROUTER_XY);
    nmc_router_init(&router_11, MULTI_CONSUMER_CORE, (NmcMeshCoordinate){.x = 1, .y = 1}, NMC_ROUTER_XY);

    const int32_t multi_thresholds[] = {1, 1, 1, 1, 1, 1, 1, 1};
    int16_t multi_weights[MULTI_WIDTH * MULTI_WIDTH];
    fill_weights(multi_weights, ARRAY_COUNT(multi_weights));

    const NmcInputGroupMappingSpec multi_inputs[] = {
        [MULTI_INPUT] = NMC_INPUT_GROUP(MULTI_WIDTH),
    };

    NmcCore observer_core;
    const NmcInputConnectionMappingSpec observer_output_inputs[] = {
        NMC_INPUT_CONNECTION(MULTI_INPUT, multi_weights),
    };
    const NmcOutputGroupMappingSpec observer_outputs[] = {
        {
            .width = MULTI_WIDTH,
            .thresholds = multi_thresholds,
            .accumulator_start = NMC_AUTO_ACCUMULATOR_OFFSET,
            .inputs = observer_output_inputs,
            .input_count = ARRAY_COUNT(observer_output_inputs),
        },
    };
    const NmcCoreMappingSpec observer_mapping = {
        .input_groups = multi_inputs,
        .input_group_count = ARRAY_COUNT(multi_inputs),
        .output_groups = observer_outputs,
        .output_group_count = ARRAY_COUNT(observer_outputs),
    };
    CHECK(nmc_core_configure_mapping(&observer_core, MULTI_OBSERVER_CORE, &observer_mapping));

    NmcCore consumer_core;
    const NmcInputConnectionMappingSpec consumer_output_inputs[] = {
        NMC_INPUT_CONNECTION(MULTI_INPUT, multi_weights),
    };
    const NmcOutputSuccessorMappingSpec consumer_successors[] = {
        NMC_SUCCESSOR(MULTI_OBSERVER_CORE, MULTI_INPUT),
    };
    const NmcPredecessorMappingSpec consumer_predecessors[] = {
        NMC_PREDECESSOR(MULTI_SOURCE_CORE, MULTI_ACK_FROM_FIRST_SUCCESSOR),
    };
    const NmcOutputGroupMappingSpec consumer_outputs[] = {
        {
            .width = MULTI_WIDTH,
            .thresholds = multi_thresholds,
            .accumulator_start = NMC_AUTO_ACCUMULATOR_OFFSET,
            .inputs = consumer_output_inputs,
            .input_count = ARRAY_COUNT(consumer_output_inputs),
            .successors = consumer_successors,
            .successor_count = ARRAY_COUNT(consumer_successors),
            .predecessors = consumer_predecessors,
            .predecessor_count = ARRAY_COUNT(consumer_predecessors),
        },
    };
    const NmcCoreMappingSpec consumer_mapping = {
        .input_groups = multi_inputs,
        .input_group_count = ARRAY_COUNT(multi_inputs),
        .output_groups = consumer_outputs,
        .output_group_count = ARRAY_COUNT(consumer_outputs),
    };
    CHECK(nmc_core_configure_mapping(&consumer_core, MULTI_CONSUMER_CORE, &consumer_mapping));

    NmcCore source_core;
    const NmcInputConnectionMappingSpec source_output_inputs[] = {
        NMC_INPUT_CONNECTION(MULTI_INPUT, multi_weights),
    };
    const NmcOutputSuccessorMappingSpec source_successors[] = {
        NMC_SUCCESSOR(MULTI_CONSUMER_CORE, MULTI_INPUT),
    };
    const NmcOutputGroupMappingSpec source_outputs[] = {
        {
            .width = MULTI_WIDTH,
            .thresholds = multi_thresholds,
            .accumulator_start = NMC_AUTO_ACCUMULATOR_OFFSET,
            .inputs = source_output_inputs,
            .input_count = ARRAY_COUNT(source_output_inputs),
            .successors = source_successors,
            .successor_count = ARRAY_COUNT(source_successors),
        },
    };
    const NmcCoreMappingSpec source_mapping = {
        .input_groups = multi_inputs,
        .input_group_count = ARRAY_COUNT(multi_inputs),
        .output_groups = source_outputs,
        .output_group_count = ARRAY_COUNT(source_outputs),
    };
    CHECK(nmc_core_configure_mapping(&source_core, MULTI_SOURCE_CORE, &source_mapping));

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
