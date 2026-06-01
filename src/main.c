#include "neuromorphic_core.h"

#include <stdio.h>
#include <stdlib.h>

#define CHECK(call) do { \
    if (!(call)) { \
        fprintf(stderr, "configuration or runtime error: %s\n", #call); \
        return EXIT_FAILURE; \
    } \
} while (0)

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

static void drain_outputs(NmcCore *core)
{
    NmcNetworkTile tile;
    while (nmc_core_pop_output_tile(core, &tile)) {
        print_network_tile(&tile);
        print_router_delivery(&tile);
    }
}

static void drain_acks(NmcCore *core)
{
    NmcAckMessage ack;
    while (nmc_core_pop_ack(core, &ack)) {
        printf("ack destination={core_id=%u, group_index=%u} completed_output_index=%u\n",
               ack.destination.core_id,
               ack.destination.group_index,
               ack.completed_output_index);
    }
}

static void print_input_tile(const NmcInputTile *tile)
{
    printf("processing input group_index=%u width=%u payload=", tile->group_index, tile->width);
    nmc_print_payload(tile->payload, tile->width);
    putchar('\n');
}

int main(void)
{
    enum {
        CORE_ID = 0,
        INPUT_A = 0,
        INPUT_B = 1,
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
     *   offset  0: OUTPUT_X x INPUT_A, 8 outputs * 8 inputs
     *   offset 64: OUTPUT_X x INPUT_B, 8 outputs * 8 inputs
     *   offset 128: OUTPUT_Y x INPUT_A, 8 outputs * 8 inputs
     */
    int16_t weights[WEIGHT_COUNT];
    for (size_t i = 0; i < WEIGHT_COUNT; ++i) {
        weights[i] = 1;
    }

    NmcCore core;
    nmc_core_init(&core, CORE_ID, weights, sizeof(weights) / sizeof(weights[0]));

    CHECK(nmc_core_add_input_group(&core));
    CHECK(nmc_core_add_input_group(&core));

    const int32_t thresholds_x[] = {2, 2, 2, 2, 2, 2, 2, 2};
    const int32_t thresholds_y[] = {1, 1, 1, 1, 1, 1, 1, 1};
    CHECK(nmc_core_add_output_group(&core, OUTPUT_X_WIDTH, thresholds_x));
    CHECK(nmc_core_add_output_group(&core, OUTPUT_Y_WIDTH, thresholds_y));

    CHECK(nmc_core_add_output_successor_lut_entry(&core, 1, 0));
    CHECK(nmc_core_add_output_successor_lut_entry(&core, 2, 0));
    CHECK(nmc_core_add_output_predecessor_lut_entry(&core, CORE_ID, INPUT_A));
    CHECK(nmc_core_add_output_predecessor_lut_entry(&core, CORE_ID, INPUT_B));
    CHECK(nmc_core_add_output_successor_lut_entry(&core, 3, 1));
    CHECK(nmc_core_add_output_predecessor_lut_entry(&core, CORE_ID, INPUT_A));

    CHECK(nmc_core_set_output_lut_range(&core, OUTPUT_X, 0, 2, 4));
    CHECK(nmc_core_set_output_lut_range(&core, OUTPUT_Y, 4, 5, 6));

    CHECK(nmc_core_add_input_output_pair_lut_entry(&core, OUTPUT_X, OUTPUT_X_INPUT_A_OFFSET));
    CHECK(nmc_core_add_input_output_pair_lut_entry(&core, OUTPUT_Y, OUTPUT_Y_INPUT_A_OFFSET));
    CHECK(nmc_core_add_input_output_pair_lut_entry(&core, OUTPUT_X, OUTPUT_X_INPUT_B_OFFSET));

    CHECK(nmc_core_set_input_lut_range(&core, INPUT_A, 0, 2));
    CHECK(nmc_core_set_input_lut_range(&core, INPUT_B, 2, 3));

    const NmcInputTile tile_a = {
        .width = INPUT_WIDTH,
        .group_index = INPUT_A,
        .payload = {0x15u},
    };
    const NmcInputTile tile_b = {
        .width = INPUT_WIDTH,
        .group_index = INPUT_B,
        .payload = {0x2Au},
    };

    print_input_tile(&tile_a);
    CHECK(nmc_core_process_input_tile(&core, &tile_a));
    drain_outputs(&core);
    drain_acks(&core);

    print_input_tile(&tile_b);
    CHECK(nmc_core_process_input_tile(&core, &tile_b));
    drain_outputs(&core);
    drain_acks(&core);

    return EXIT_SUCCESS;
}
