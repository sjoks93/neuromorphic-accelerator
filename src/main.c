#include "neuromorphic_core.h"

#include <stdio.h>
#include <stdlib.h>

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

/* Drain ACK queue and return how many predecessor ACKs were generated. */
static size_t drain_acks(NmcCore *core)
{
    size_t count = 0u;
    NmcAckMessage ack;
    while (nmc_core_pop_ack(core, &ack)) {
        ++count;
        printf("ack destination={core_id=%u, group_index=%u} completed_output_index=%u\n",
               ack.destination.core_id,
               ack.destination.group_index,
               ack.completed_output_index);
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
        .destination = {
            .core_id = core->core_id,
            .group_index = ack_index,
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
    CHECK(drain_outputs(&core) == 1u);
    CHECK(drain_acks(&core) == 1u);
    print_output_counters(&core);

    begin_step("natural step 0: input B arrives, so X now fires");
    print_input_tile(&step0_b);
    CHECK(nmc_core_process_input_tile(&core, &step0_b));
    CHECK(drain_outputs(&core) == 1u);
    CHECK(drain_acks(&core) == 2u);
    print_output_counters(&core);

    begin_step("natural step 1: predecessor ACKs arrived, so input A can arrive before successor ACKs");
    print_input_tile(&step1_a);
    CHECK(nmc_core_process_input_tile(&core, &step1_a));
    CHECK(drain_outputs(&core) == 0u);
    CHECK(drain_acks(&core) == 0u);
    print_output_counters(&core);

    begin_step("natural step 1: input B is consumed, but X still waits for successor ACKs");
    print_input_tile(&step1_b);
    CHECK(nmc_core_process_input_tile(&core, &step1_b));
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
    CHECK(drain_acks(&core) == 2u);
    print_output_counters(&core);

    begin_step("natural step 2: predecessor ACKs arrived; input B arrives first before successor ACKs");
    print_input_tile(&step2_b);
    CHECK(nmc_core_process_input_tile(&core, &step2_b));
    CHECK(drain_outputs(&core) == 0u);
    CHECK(drain_acks(&core) == 0u);
    print_output_counters(&core);

    begin_step("natural step 2: input A arrives second; both groups are complete but ACK-blocked");
    print_input_tile(&step2_a);
    CHECK(nmc_core_process_input_tile(&core, &step2_a));
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
    CHECK(drain_acks(&core) == 2u);
    print_output_counters(&core);

    begin_step("test bench passed");
    return EXIT_SUCCESS;
}
