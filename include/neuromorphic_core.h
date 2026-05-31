#ifndef NEUROMORPHIC_CORE_H
#define NEUROMORPHIC_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NMC_MAX_GROUP_NEURONS 256u
#define NMC_BITS_PER_BYTE 8u
#define NMC_MAX_GROUP_BYTES (NMC_MAX_GROUP_NEURONS / NMC_BITS_PER_BYTE)
#define NMC_MAX_INPUT_GROUPS 16u
#define NMC_MAX_OUTPUT_GROUPS 16u
#define NMC_MAX_INPUT_OUTPUT_PAIR_LUT_ENTRIES 64u
#define NMC_MAX_OUTPUT_DESTINATION_LUT_ENTRIES 64u
#define NMC_MAX_TILE_DESTINATIONS 8u
#define NMC_MAX_OUTPUT_QUEUE 64u
#define NMC_INVALID_INDEX UINT32_MAX

typedef uint32_t nmc_core_id_t;
typedef uint32_t nmc_input_index_t;
typedef uint32_t nmc_output_index_t;
typedef uint16_t nmc_tile_width_t;

typedef struct {
    bool valid;
    size_t start;
    size_t end;
} NmcIndirectAddress;

typedef struct {
    int32_t accumulator;
    int32_t threshold;
} NmcNeuron;

typedef struct {
    nmc_tile_width_t width;
    NmcIndirectAddress lut;
} NmcInputGroup;

typedef struct {
    nmc_tile_width_t width;
    uint32_t input_count;
    uint32_t remaining_input_count;
    NmcNeuron neurons[NMC_MAX_GROUP_NEURONS];
    NmcIndirectAddress lut;
} NmcOutputGroup;

typedef struct {
    nmc_output_index_t output_index;
    size_t weight_offset;
} NmcInputOutputPairLutEntry;

typedef struct {
    nmc_core_id_t core_id;
    nmc_input_index_t group_index;
} NmcDestinationAddress;

typedef struct {
    nmc_tile_width_t width;
    nmc_input_index_t group_index;
    uint8_t payload[NMC_MAX_GROUP_BYTES];
} NmcInputTile;

typedef struct {
    uint8_t destination_count;
    nmc_tile_width_t width;
    NmcDestinationAddress destinations[NMC_MAX_TILE_DESTINATIONS];
    uint8_t payload[NMC_MAX_GROUP_BYTES];
} NmcNetworkTile;

typedef struct {
    nmc_core_id_t core_id;

    NmcInputGroup input_groups[NMC_MAX_INPUT_GROUPS];
    size_t input_group_count;

    NmcOutputGroup output_groups[NMC_MAX_OUTPUT_GROUPS];
    size_t output_group_count;

    NmcInputOutputPairLutEntry input_output_pair_lut[NMC_MAX_INPUT_OUTPUT_PAIR_LUT_ENTRIES];
    size_t input_output_pair_lut_count;

    NmcDestinationAddress output_destination_lut[NMC_MAX_OUTPUT_DESTINATION_LUT_ENTRIES];
    size_t output_destination_lut_count;

    int16_t *weights;
    size_t weight_count;

    NmcNetworkTile output_queue[NMC_MAX_OUTPUT_QUEUE];
    size_t output_queue_count;
} NmcCore;

void nmc_core_init(NmcCore *core, nmc_core_id_t core_id, int16_t *weights, size_t weight_count);
bool nmc_core_add_input_group(NmcCore *core, nmc_tile_width_t width);
bool nmc_core_add_output_group(NmcCore *core, nmc_tile_width_t width, const int32_t *thresholds);
bool nmc_core_set_output_lut_range(NmcCore *core,
                                   nmc_output_index_t output_index,
                                   size_t destination_start,
                                   size_t destination_end);
bool nmc_core_add_output_destination_lut_entry(NmcCore *core,
                                               nmc_core_id_t target_core_id,
                                               nmc_input_index_t target_group_index);
bool nmc_core_set_input_lut_range(NmcCore *core,
                                  nmc_input_index_t input_index,
                                  size_t pair_start,
                                  size_t pair_end);
bool nmc_core_add_input_output_pair_lut_entry(NmcCore *core,
                                              nmc_output_index_t output_index,
                                              size_t weight_offset);
bool nmc_core_process_input_tile(NmcCore *core, const NmcInputTile *tile);
bool nmc_core_pop_output_tile(NmcCore *core, NmcNetworkTile *tile);
bool nmc_network_tile_get_input_tile(const NmcNetworkTile *network_tile, size_t destination_index, NmcInputTile *input_tile);
void nmc_print_payload(const uint8_t *payload, nmc_tile_width_t width);

#endif
