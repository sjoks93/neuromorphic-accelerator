#ifndef NMC_INTERNAL_CORE_H
#define NMC_INTERNAL_CORE_H

#include "nmc/core.h"

#include <stddef.h>
#include <stdint.h>

/* Private core implementation interface. Do not include from application code. */
typedef struct {
    nmc_tile_width_t events[NMC_MAX_GROUP_NEURONS];
    uint32_t event_count;
    uint64_t encoder_cycles;
} NmcInputLaneEvents;

typedef enum {
    NMC_MEMORY_OUTPUT_TO_ADDER_TREE,
    NMC_MEMORY_OUTPUT_TO_ACCUMULATOR_MUX,
} NmcMemoryOutputRoute;

#define NMC_ACTIVATION_PROGRAM_IF_IMMEDIATE 0u
#define NMC_ACTIVATION_PROGRAM_IF_SRAM_THRESHOLD 1u
#define NMC_ACTIVATION_THRESHOLD_IMMEDIATE 0u
#define NMC_ACTIVATION_RESET_IMMEDIATE 1u
#define NMC_ACTIVATION_THRESHOLD_RANGE 0u

bool nmc_core_valid_input_index(const NmcCore *core, nmc_input_index_t input_index);
bool nmc_core_valid_ack_index(const NmcCore *core, nmc_ack_index_t ack_index);
bool nmc_core_valid_output_index(const NmcCore *core, nmc_output_index_t output_index);
bool nmc_core_valid_width(nmc_tile_width_t width);
size_t nmc_core_payload_bytes(nmc_tile_width_t width);
bool nmc_core_payload_bit_is_set(const uint8_t *payload, size_t bit_index);
void nmc_core_payload_set_bit(uint8_t *payload, size_t bit_index);
bool nmc_core_output_accumulators_fit(const NmcCore *core,
                                      nmc_output_index_t output_index,
                                      size_t *accumulator_start);
size_t nmc_core_accumulator_lane_span(nmc_tile_width_t output_width);
size_t nmc_core_accumulator_lane_address(size_t accumulator_start, size_t output_index);
size_t nmc_core_accumulator_mux_index(size_t accumulator_lane_address);
bool nmc_core_memory_read_weight_lane(const NmcCore *core,
                                      size_t lane_address,
                                      NmcMemoryOutputRoute route,
                                      int16_t *value);
bool nmc_core_memory_read_accumulator(const NmcCore *core,
                                      size_t accumulator_start,
                                      size_t output_index,
                                      NmcMemoryOutputRoute route,
                                      int32_t *value);
bool nmc_core_memory_write_accumulator(NmcCore *core,
                                       size_t accumulator_start,
                                       size_t output_index,
                                       int32_t value);
bool nmc_core_memory_read_activation_word(const NmcCore *core,
                                          size_t lane_address,
                                          int32_t *value);
bool nmc_core_memory_write_activation_word(NmcCore *core,
                                           size_t lane_address,
                                           int32_t value);

bool nmc_core_encode_input_events(const uint8_t *input_payload,
                                  nmc_tile_width_t input_width,
                                  NmcInputLaneEvents *lane_events,
                                  uint32_t *event_count,
                                  uint64_t *encoder_cycles);

bool nmc_core_pair_weights_fit(const NmcCore *core,
                               nmc_tile_width_t input_width,
                               const NmcInputOutputPairLutEntry *pair_entry);
bool nmc_core_accumulate_pair(NmcCore *core,
                              nmc_tile_width_t input_width,
                              const NmcInputOutputPairLutEntry *pair_entry,
                              NmcOutputGroup *output_group,
                              const NmcInputLaneEvents *lane_events);

bool nmc_core_output_group_ready(const NmcOutputGroup *group);
size_t nmc_core_output_group_successor_count(const NmcOutputGroup *group);
bool nmc_core_execute_activation_program(NmcCore *core,
                                         nmc_output_index_t output_index,
                                         size_t accumulator_start,
                                         uint8_t *payload);
bool nmc_core_activate_output_group(NmcCore *core, nmc_output_index_t output_index);

#endif
