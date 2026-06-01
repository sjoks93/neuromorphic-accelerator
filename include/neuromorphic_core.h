#ifndef NEUROMORPHIC_CORE_H
#define NEUROMORPHIC_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Static limits keep the model hardware-like: arrays have fixed bounds and no
 * dynamic allocation is needed in the core datapath.
 */
#define NMC_MAX_GROUP_NEURONS 256u
#define NMC_BITS_PER_BYTE 8u
#define NMC_MAX_GROUP_BYTES (NMC_MAX_GROUP_NEURONS / NMC_BITS_PER_BYTE)
#define NMC_MAX_INPUT_GROUPS 16u
#define NMC_MAX_INPUT_LUT_STARTS (NMC_MAX_INPUT_GROUPS + 1u)
#define NMC_MAX_ACK_GROUPS 16u
#define NMC_MAX_ACK_LUT_STARTS (NMC_MAX_ACK_GROUPS + 1u)
#define NMC_MAX_OUTPUT_GROUPS 16u
#define NMC_MAX_OUTPUT_LUT_STARTS (NMC_MAX_OUTPUT_GROUPS + 1u)
#define NMC_MAX_INPUT_OUTPUT_PAIR_LUT_ENTRIES 64u
#define NMC_MAX_ACK_OUTPUT_PAIR_LUT_ENTRIES 64u
#define NMC_MAX_OUTPUT_ROUTE_LUT_ENTRIES 64u
#define NMC_MAX_TILE_DESTINATIONS 8u
#define NMC_MAX_OUTPUT_QUEUE 64u
#define NMC_MAX_ACK_QUEUE 64u
#define NMC_INVALID_INDEX UINT32_MAX

typedef uint32_t nmc_core_id_t;
typedef uint32_t nmc_input_index_t;
typedef uint32_t nmc_ack_index_t;
typedef uint32_t nmc_output_index_t;
typedef uint16_t nmc_tile_width_t;

/*
 * Generic first-stage indirection entry.
 *
 * The entry stores only the start address.  The exclusive end is supplied by
 * the following entry, so tables are configured as starts plus one terminal
 * sentinel entry.
 */
typedef struct {
    bool valid;
    size_t start;
} NmcIndirectAddress;

/*
 * First-stage output route entry.
 *
 * Successor entries are in [successor_start, predecessor_start).  Predecessor
 * ACK entries are in [predecessor_start, next_output.successor_start).  The
 * final output group is terminated by a sentinel output entry.
 */
typedef struct {
    bool valid;
    nmc_tile_width_t bitmap_width;
    size_t successor_start;
    size_t predecessor_start;
} NmcOutputRouteAddress;

/* Per-output-neuron state kept separate from group routing metadata. */
typedef struct {
    int32_t accumulator;
    int32_t threshold;
} NmcNeuron;

/* Input group table entry: stage-1 input LUT. */
typedef struct {
    NmcIndirectAddress lut;
} NmcInputGroup;

/* ACK group table entry: stage-1 ACK input LUT. */
typedef struct {
    NmcIndirectAddress lut;
} NmcAckGroup;

/* Output group state plus stage-1 output route LUT entry. */
typedef struct {
    uint32_t input_requirement;
    uint32_t input_count;
    uint32_t ack_count;
    NmcNeuron neurons[NMC_MAX_GROUP_NEURONS];
    NmcOutputRouteAddress route_lut;
} NmcOutputGroup;

/* Stage-2 input LUT entry: one input group contributes to one output group. */
typedef struct {
    nmc_output_index_t output_index;
    size_t weight_offset;
} NmcInputOutputPairLutEntry;

/*
 * Stage-2 ACK input LUT entry.
 *
 * An incoming ACK index may fan out to multiple output groups, incrementing
 * every listed group's ACK counter.
 */
typedef struct {
    nmc_output_index_t output_index;
} NmcAckOutputPairLutEntry;

/* Router-visible address used by output tiles and ACK messages. */
typedef struct {
    nmc_core_id_t core_id;
    nmc_input_index_t group_index;
} NmcDestinationAddress;

/*
 * Stage-2 output route LUT entry.
 *
 * Successor entries route output tiles.  Predecessor entries route ACKs back
 * to the predecessor core's ACK input indices.
 */
typedef struct {
    NmcDestinationAddress address;
} NmcOutputRouteLutEntry;

/* Router-facing ACK message: no payload, but the destination header is multicast. */
typedef struct {
    uint8_t destination_count;
    NmcDestinationAddress destinations[NMC_MAX_TILE_DESTINATIONS];
    nmc_output_index_t completed_output_index;
} NmcAckMessage;

/* Local, destination-core-facing input tile. */
typedef struct {
    nmc_tile_width_t width;
    nmc_input_index_t group_index;
    uint8_t payload[NMC_MAX_GROUP_BYTES];
} NmcInputTile;

/* Router-facing output tile: one payload multicast to multiple destinations. */
typedef struct {
    uint8_t destination_count;
    nmc_tile_width_t width;
    NmcDestinationAddress destinations[NMC_MAX_TILE_DESTINATIONS];
    uint8_t payload[NMC_MAX_GROUP_BYTES];
} NmcNetworkTile;

/* Complete simulator state for one core instance. */
typedef struct {
    nmc_core_id_t core_id;

    NmcInputGroup input_groups[NMC_MAX_INPUT_LUT_STARTS];
    size_t input_group_count;

    NmcAckGroup ack_groups[NMC_MAX_ACK_LUT_STARTS];
    size_t ack_group_count;

    NmcOutputGroup output_groups[NMC_MAX_OUTPUT_LUT_STARTS];
    size_t output_group_count;

    NmcInputOutputPairLutEntry input_output_pair_lut[NMC_MAX_INPUT_OUTPUT_PAIR_LUT_ENTRIES];
    size_t input_output_pair_lut_count;

    NmcAckOutputPairLutEntry ack_output_pair_lut[NMC_MAX_ACK_OUTPUT_PAIR_LUT_ENTRIES];
    size_t ack_output_pair_lut_count;

    NmcOutputRouteLutEntry output_route_lut[NMC_MAX_OUTPUT_ROUTE_LUT_ENTRIES];
    size_t output_route_lut_count;

    int16_t *weights;
    size_t weight_count;

    NmcNetworkTile output_queue[NMC_MAX_OUTPUT_QUEUE];
    size_t output_queue_count;

    NmcAckMessage ack_queue[NMC_MAX_ACK_QUEUE];
    size_t ack_queue_count;
} NmcCore;

/* Configuration and runtime API. */
void nmc_core_init(NmcCore *core, nmc_core_id_t core_id, int16_t *weights, size_t weight_count);
bool nmc_core_add_input_group(NmcCore *core);
bool nmc_core_add_ack_group(NmcCore *core);
bool nmc_core_add_output_group(NmcCore *core, nmc_tile_width_t width, const int32_t *thresholds);
bool nmc_core_set_output_lut_starts(NmcCore *core,
                                    nmc_output_index_t output_index,
                                    size_t successor_start,
                                    size_t predecessor_start);
bool nmc_core_add_output_successor_lut_entry(NmcCore *core,
                                             nmc_core_id_t target_core_id,
                                             nmc_input_index_t target_group_index);
bool nmc_core_add_output_predecessor_lut_entry(NmcCore *core,
                                               nmc_core_id_t predecessor_core_id,
                                               nmc_input_index_t predecessor_group_index);
bool nmc_core_set_input_lut_start(NmcCore *core,
                                  nmc_input_index_t input_index,
                                  size_t pair_start);
bool nmc_core_set_ack_lut_start(NmcCore *core,
                                nmc_ack_index_t ack_index,
                                size_t pair_start);
bool nmc_core_add_input_output_pair_lut_entry(NmcCore *core,
                                              nmc_output_index_t output_index,
                                              size_t weight_offset);
bool nmc_core_add_ack_output_pair_lut_entry(NmcCore *core,
                                            nmc_output_index_t output_index);
bool nmc_core_process_input_tile(NmcCore *core, const NmcInputTile *tile);
bool nmc_core_pop_output_tile(NmcCore *core, NmcNetworkTile *tile);
bool nmc_core_process_ack(NmcCore *core, const NmcAckMessage *ack);
bool nmc_core_flush_ready_outputs(NmcCore *core);
bool nmc_core_pop_ack(NmcCore *core, NmcAckMessage *ack);
bool nmc_network_tile_get_input_tile(const NmcNetworkTile *network_tile, size_t destination_index, NmcInputTile *input_tile);
void nmc_print_payload(const uint8_t *payload, nmc_tile_width_t width);

#endif
