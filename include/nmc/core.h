#ifndef NMC_CORE_H
#define NMC_CORE_H

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
#define NMC_MAX_MAPPED_CORES 16u
#define NMC_MAX_NETWORK_GROUPS 32u
#define NMC_MAX_NETWORK_CONNECTIONS 64u
#define NMC_MAX_NETWORK_INPUTS 32u
#define NMC_MAX_TILE_DESTINATIONS 8u
#define NMC_MAX_OUTPUT_QUEUE 64u
#define NMC_MAX_ACK_QUEUE 64u
#define NMC_EVENT_ENCODER_WINDOW 8u
#define NMC_INPUT_PARALLELISM 4u
#define NMC_OUTPUT_PARALLELISM 4u
#define NMC_WEIGHT_MEMORY_SIZE 1024u
#define NMC_ACCUMULATOR_MEMORY_SIZE 1024u
#define NMC_INVALID_INDEX UINT32_MAX
#define NMC_AUTO_WEIGHT_OFFSET SIZE_MAX
#define NMC_AUTO_ACCUMULATOR_OFFSET SIZE_MAX
#define NMC_INPUT_GROUP(width_) ((NmcInputGroupMappingSpec){.width = (width_)})
#define NMC_INPUT_CONNECTION(input_group_, weights_) ((NmcInputConnectionMappingSpec){.input_group = (input_group_), .weight_offset = NMC_AUTO_WEIGHT_OFFSET, .weights = (weights_)})
#define NMC_SUCCESSOR(core_id_, input_group_) ((NmcOutputSuccessorMappingSpec){.core_id = (core_id_), .input_group = (input_group_)})
#define NMC_PREDECESSOR(core_id_, ack_group_) ((NmcPredecessorMappingSpec){.core_id = (core_id_), .ack_group = (ack_group_)})

typedef uint32_t nmc_core_id_t;
typedef uint32_t nmc_network_group_id_t;
typedef uint32_t nmc_external_input_id_t;
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

/* Per-output-neuron threshold state kept separate from group routing metadata. */
typedef struct {
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

/* High-level declaration of one local input group. */
typedef struct {
    nmc_tile_width_t width;
} NmcInputGroupMappingSpec;

/* High-level declaration of one input edge feeding an output group. */
typedef struct {
    nmc_input_index_t input_group;
    size_t weight_offset;
    const int16_t *weights;
} NmcInputConnectionMappingSpec;

/* High-level declaration of one successor edge driven by an output group. */
typedef struct {
    nmc_core_id_t core_id;
    nmc_input_index_t input_group;
} NmcOutputSuccessorMappingSpec;

/* High-level declaration of one predecessor edge that receives ACKs from an output group. */
typedef struct {
    nmc_core_id_t core_id;
    nmc_ack_index_t ack_group;
} NmcPredecessorMappingSpec;

/* High-level output group mapping specification. */
typedef struct {
    nmc_tile_width_t width;
    const int32_t *thresholds;
    size_t accumulator_start;
    const NmcInputConnectionMappingSpec *inputs;
    size_t input_count;
    const NmcOutputSuccessorMappingSpec *successors;
    size_t successor_count;
    const NmcPredecessorMappingSpec *predecessors;
    size_t predecessor_count;
} NmcOutputGroupMappingSpec;

/* Complete high-level mapping specification for one core. */
typedef struct {
    const NmcInputGroupMappingSpec *input_groups;
    size_t input_group_count;
    const NmcOutputGroupMappingSpec *output_groups;
    size_t output_group_count;
} NmcCoreMappingSpec;

/* Graph-level declaration of one logical neural group placed on one core. */
typedef struct {
    nmc_network_group_id_t group_id;
    nmc_core_id_t core_id;
    nmc_tile_width_t width;
    const int32_t *thresholds;
} NmcNetworkGroupMappingSpec;

/* Graph-level declaration of one side/input-interface edge into a logical group. */
typedef struct {
    nmc_external_input_id_t input_id;
    nmc_network_group_id_t destination_group;
    nmc_tile_width_t width;
    const int16_t *weights;
} NmcNetworkInputMappingSpec;

/* Graph-level declaration of one logical group-to-group edge. */
typedef struct {
    nmc_network_group_id_t source_group;
    nmc_network_group_id_t destination_group;
    const int16_t *weights;
} NmcNetworkConnectionMappingSpec;

/* Minimal graph-level mapping: logical groups, optional external inputs, and connections. */
typedef struct {
    const NmcNetworkGroupMappingSpec *groups;
    size_t group_count;
    const NmcNetworkInputMappingSpec *inputs;
    size_t input_count;
    const NmcNetworkConnectionMappingSpec *connections;
    size_t connection_count;
} NmcNetworkMappingSpec;

/* Generated, owned storage for one core-local mapping. */
typedef struct {
    nmc_core_id_t core_id;
    NmcCoreMappingSpec mapping;
    NmcInputGroupMappingSpec input_groups[NMC_MAX_INPUT_GROUPS];
    NmcOutputGroupMappingSpec output_groups[NMC_MAX_OUTPUT_GROUPS];
    NmcInputConnectionMappingSpec output_inputs[NMC_MAX_OUTPUT_GROUPS][NMC_MAX_INPUT_GROUPS];
    NmcOutputSuccessorMappingSpec output_successors[NMC_MAX_OUTPUT_GROUPS][NMC_MAX_OUTPUT_ROUTE_LUT_ENTRIES];
    NmcPredecessorMappingSpec output_predecessors[NMC_MAX_OUTPUT_GROUPS][NMC_MAX_OUTPUT_ROUTE_LUT_ENTRIES];
    nmc_network_group_id_t output_group_ids[NMC_MAX_OUTPUT_GROUPS];
    bool input_is_external[NMC_MAX_INPUT_GROUPS];
    nmc_external_input_id_t input_external_ids[NMC_MAX_INPUT_GROUPS];
    nmc_network_group_id_t input_source_group_ids[NMC_MAX_INPUT_GROUPS];
    nmc_network_group_id_t input_destination_group_ids[NMC_MAX_INPUT_GROUPS];
} NmcGeneratedCoreMapping;

/* Generated mappings for every core used by a graph-level mapping. */
typedef struct {
    size_t core_count;
    NmcGeneratedCoreMapping cores[NMC_MAX_MAPPED_CORES];
} NmcGeneratedMappings;

/* Complete simulator state for one core instance. */
typedef struct {
    nmc_core_id_t core_id;

    NmcInputGroup input_groups[NMC_MAX_INPUT_LUT_STARTS];
    size_t input_group_count;

    NmcAckGroup ack_groups[NMC_MAX_ACK_LUT_STARTS];
    size_t ack_group_count;

    NmcOutputGroup output_groups[NMC_MAX_OUTPUT_LUT_STARTS];
    size_t output_group_count;

    NmcIndirectAddress accumulator_lut[NMC_MAX_OUTPUT_LUT_STARTS];

    NmcInputOutputPairLutEntry input_output_pair_lut[NMC_MAX_INPUT_OUTPUT_PAIR_LUT_ENTRIES];
    size_t input_output_pair_lut_count;

    NmcAckOutputPairLutEntry ack_output_pair_lut[NMC_MAX_ACK_OUTPUT_PAIR_LUT_ENTRIES];
    size_t ack_output_pair_lut_count;

    NmcOutputRouteLutEntry output_route_lut[NMC_MAX_OUTPUT_ROUTE_LUT_ENTRIES];
    size_t output_route_lut_count;

    int16_t weights[NMC_WEIGHT_MEMORY_SIZE];
    int32_t accumulators[NMC_ACCUMULATOR_MEMORY_SIZE];

    NmcNetworkTile output_queue[NMC_MAX_OUTPUT_QUEUE];
    size_t output_queue_count;

    NmcAckMessage ack_queue[NMC_MAX_ACK_QUEUE];
    size_t ack_queue_count;

    /* Compute-schedule counters: one input-lane adder-tree reduction per output-lane block per cycle. */
    uint64_t total_compute_cycles;
    uint64_t last_input_tile_compute_cycles;
    uint64_t total_encoder_cycles;
    uint64_t last_input_tile_encoder_cycles;
    uint32_t last_input_tile_event_count;
} NmcCore;

/* Configuration and runtime API. */
void nmc_core_init(NmcCore *core, nmc_core_id_t core_id);
bool nmc_core_add_input_group(NmcCore *core);
bool nmc_core_add_ack_group(NmcCore *core);
bool nmc_core_add_output_group(NmcCore *core, nmc_tile_width_t width, const int32_t *thresholds);
bool nmc_core_set_output_accumulator_lut_start(NmcCore *core,
                                               nmc_output_index_t output_index,
                                               size_t accumulator_start);
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
bool nmc_core_configure_mapping(NmcCore *core, nmc_core_id_t core_id, const NmcCoreMappingSpec *mapping);
bool nmc_core_mapping_ack_group_for_successor(const NmcCoreMappingSpec *mapping,
                                              nmc_core_id_t successor_core_id,
                                              nmc_input_index_t successor_input_group,
                                              nmc_ack_index_t *ack_group);
bool nmc_generate_core_mappings(const NmcNetworkMappingSpec *network, NmcGeneratedMappings *generated);
const NmcGeneratedCoreMapping *nmc_generated_mappings_find_core(const NmcGeneratedMappings *generated,
                                                                nmc_core_id_t core_id);
bool nmc_core_process_input_tile(NmcCore *core, const NmcInputTile *tile);
bool nmc_core_pop_output_tile(NmcCore *core, NmcNetworkTile *tile);
bool nmc_core_process_ack(NmcCore *core, const NmcAckMessage *ack);
bool nmc_core_flush_ready_outputs(NmcCore *core);
bool nmc_core_pop_ack(NmcCore *core, NmcAckMessage *ack);
bool nmc_network_tile_get_input_tile(const NmcNetworkTile *network_tile, size_t destination_index, NmcInputTile *input_tile);
void nmc_print_payload(const uint8_t *payload, nmc_tile_width_t width);

#endif
