#include "nmc/internal/core.h"

#include <string.h>

typedef struct {
    nmc_core_id_t core_id;
    nmc_input_index_t input_group;
} NmcSuccessorEndpoint;

typedef struct {
    NmcGeneratedCoreMapping *core;
    nmc_output_index_t output_index;
} NmcGeneratedGroupLocation;

static bool valid_spec_array(const void *items, size_t count)
{
    return count == 0u || items != NULL;
}

static NmcGeneratedCoreMapping *generated_find_core(NmcGeneratedMappings *generated, nmc_core_id_t core_id)
{
    for (size_t i = 0u; i < generated->core_count; ++i) {
        if (generated->cores[i].core_id == core_id) {
            return &generated->cores[i];
        }
    }
    return NULL;
}

const NmcGeneratedCoreMapping *nmc_generated_mappings_find_core(const NmcGeneratedMappings *generated,
                                                                nmc_core_id_t core_id)
{
    if (generated == NULL) {
        return NULL;
    }

    for (size_t i = 0u; i < generated->core_count; ++i) {
        if (generated->cores[i].core_id == core_id) {
            return &generated->cores[i];
        }
    }
    return NULL;
}

static NmcGeneratedCoreMapping *generated_get_or_add_core(NmcGeneratedMappings *generated, nmc_core_id_t core_id)
{
    NmcGeneratedCoreMapping *core = generated_find_core(generated, core_id);
    if (core != NULL) {
        return core;
    }
    if (generated->core_count >= NMC_MAX_MAPPED_CORES) {
        return NULL;
    }

    core = &generated->cores[generated->core_count++];
    memset(core, 0, sizeof(*core));
    core->core_id = core_id;
    core->mapping.input_groups = core->input_groups;
    core->mapping.output_groups = core->output_groups;
    return core;
}

static bool network_find_group(const NmcNetworkMappingSpec *network,
                               nmc_network_group_id_t group_id,
                               size_t *group_index)
{
    for (size_t i = 0u; i < network->group_count; ++i) {
        if (network->groups[i].group_id == group_id) {
            *group_index = i;
            return true;
        }
    }
    return false;
}

static bool network_group_reaches(const NmcNetworkMappingSpec *network,
                                  nmc_network_group_id_t current_group,
                                  nmc_network_group_id_t target_group,
                                  bool *visited)
{
    if (current_group == target_group) {
        return true;
    }

    size_t current_index = 0u;
    if (!network_find_group(network, current_group, &current_index)) {
        return false;
    }
    if (visited[current_index]) {
        return false;
    }
    visited[current_index] = true;

    for (size_t i = 0u; i < network->connection_count; ++i) {
        if (network->connections[i].source_group == current_group &&
            network_group_reaches(network, network->connections[i].destination_group, target_group, visited)) {
            return true;
        }
    }
    return false;
}

static bool network_connection_is_recurrent(const NmcNetworkMappingSpec *network,
                                            const NmcNetworkConnectionMappingSpec *connection)
{
    if (connection->recurrent || connection->source_group == connection->destination_group) {
        return true;
    }

    bool visited[NMC_MAX_NETWORK_GROUPS] = {false};
    return network_group_reaches(network, connection->destination_group, connection->source_group, visited);
}

static bool generated_find_group_location(NmcGeneratedMappings *generated,
                                          nmc_network_group_id_t group_id,
                                          NmcGeneratedGroupLocation *location)
{
    for (size_t core_index = 0u; core_index < generated->core_count; ++core_index) {
        NmcGeneratedCoreMapping *core = &generated->cores[core_index];
        for (size_t output_index = 0u; output_index < core->mapping.output_group_count; ++output_index) {
            if (core->output_group_ids[output_index] == group_id) {
                location->core = core;
                location->output_index = (nmc_output_index_t)output_index;
                return true;
            }
        }
    }
    return false;
}

static bool add_generated_output_input_entry(NmcGeneratedCoreMapping *destination_core,
                                             nmc_output_index_t destination_output,
                                             nmc_input_index_t local_input,
                                             size_t weight_offset,
                                             const int16_t *weights,
                                             bool recurrent)
{
    if ((size_t)destination_output >= destination_core->mapping.output_group_count ||
        (size_t)local_input >= destination_core->mapping.input_group_count) {
        return false;
    }

    NmcOutputGroupMappingSpec *output = &destination_core->output_groups[destination_output];
    if (output->input_count >= NMC_MAX_INPUT_GROUPS) {
        return false;
    }
    for (size_t i = 0u; i < output->input_count; ++i) {
        if (output->inputs[i].input_group == local_input) {
            return false;
        }
    }

    destination_core->output_inputs[destination_output][output->input_count] = (NmcInputConnectionMappingSpec){
        .input_group = local_input,
        .weight_offset = weight_offset,
        .weights = weights,
        .recurrent = recurrent,
    };
    output->inputs = destination_core->output_inputs[destination_output];
    ++output->input_count;
    return true;
}

static bool add_generated_input_connection(NmcGeneratedCoreMapping *destination_core,
                                           nmc_output_index_t destination_output,
                                           nmc_tile_width_t input_width,
                                           nmc_network_group_id_t source_group,
                                           nmc_network_group_id_t destination_group,
                                           size_t weight_offset,
                                           const int16_t *weights,
                                           bool recurrent,
                                           nmc_input_index_t *input_group)
{
    if (destination_core->mapping.input_group_count >= NMC_MAX_INPUT_GROUPS ||
        (size_t)destination_output >= destination_core->mapping.output_group_count) {
        return false;
    }

    const nmc_input_index_t local_input = (nmc_input_index_t)destination_core->mapping.input_group_count;
    destination_core->input_groups[local_input] = NMC_INPUT_GROUP(input_width);
    destination_core->input_source_group_ids[local_input] = source_group;
    destination_core->input_destination_group_ids[local_input] = destination_group;
    ++destination_core->mapping.input_group_count;

    if (!add_generated_output_input_entry(destination_core, destination_output, local_input, weight_offset, weights, recurrent)) {
        return false;
    }
    *input_group = local_input;
    return true;
}

static bool find_generated_external_input(const NmcGeneratedCoreMapping *destination_core,
                                          nmc_external_input_id_t input_id,
                                          nmc_tile_width_t input_width,
                                          nmc_input_index_t *input_group)
{
    for (size_t i = 0u; i < destination_core->mapping.input_group_count; ++i) {
        if (destination_core->input_is_external[i] && destination_core->input_external_ids[i] == input_id) {
            if (destination_core->input_groups[i].width != input_width) {
                return false;
            }
            *input_group = (nmc_input_index_t)i;
            return true;
        }
    }

    *input_group = NMC_INVALID_INDEX;
    return true;
}

static bool add_generated_external_input_connection(NmcGeneratedCoreMapping *destination_core,
                                                    nmc_output_index_t destination_output,
                                                    nmc_external_input_id_t input_id,
                                                    nmc_tile_width_t input_width,
                                                    nmc_network_group_id_t destination_group,
                                                    const int16_t *weights,
                                                    nmc_input_index_t *input_group)
{
    nmc_input_index_t local_input = NMC_INVALID_INDEX;
    if (!find_generated_external_input(destination_core, input_id, input_width, &local_input)) {
        return false;
    }

    if (local_input == NMC_INVALID_INDEX) {
        if (destination_core->mapping.input_group_count >= NMC_MAX_INPUT_GROUPS) {
            return false;
        }
        local_input = (nmc_input_index_t)destination_core->mapping.input_group_count;
        destination_core->input_groups[local_input] = NMC_INPUT_GROUP(input_width);
        destination_core->input_is_external[local_input] = true;
        destination_core->input_external_ids[local_input] = input_id;
        destination_core->input_source_group_ids[local_input] = NMC_INVALID_INDEX;
        destination_core->input_destination_group_ids[local_input] = destination_group;
        ++destination_core->mapping.input_group_count;
    }

    if (!add_generated_output_input_entry(destination_core,
                                          destination_output,
                                          local_input,
                                          NMC_AUTO_WEIGHT_OFFSET,
                                          weights,
                                          false)) {
        return false;
    }

    *input_group = local_input;
    return true;
}

static bool add_generated_successor(NmcGeneratedCoreMapping *source_core,
                                    nmc_output_index_t source_output,
                                    nmc_core_id_t destination_core_id,
                                    nmc_input_index_t destination_input)
{
    if ((size_t)source_output >= source_core->mapping.output_group_count) {
        return false;
    }

    NmcOutputGroupMappingSpec *output = &source_core->output_groups[source_output];
    if (output->successor_count >= NMC_MAX_OUTPUT_ROUTE_LUT_ENTRIES || output->successor_count >= NMC_MAX_TILE_DESTINATIONS) {
        return false;
    }

    source_core->output_successors[source_output][output->successor_count] = NMC_SUCCESSOR(destination_core_id, destination_input);
    output->successors = source_core->output_successors[source_output];
    ++output->successor_count;
    return true;
}

static bool add_generated_predecessor(NmcGeneratedCoreMapping *destination_core,
                                      nmc_output_index_t destination_output,
                                      nmc_core_id_t source_core_id,
                                      nmc_ack_index_t source_ack_group,
                                      bool recurrent)
{
    if ((size_t)destination_output >= destination_core->mapping.output_group_count) {
        return false;
    }

    NmcOutputGroupMappingSpec *output = &destination_core->output_groups[destination_output];
    if (output->predecessor_count >= NMC_MAX_OUTPUT_ROUTE_LUT_ENTRIES || output->predecessor_count >= NMC_MAX_TILE_DESTINATIONS) {
        return false;
    }

    destination_core->output_predecessors[destination_output][output->predecessor_count] = (NmcPredecessorMappingSpec){
        .core_id = source_core_id,
        .ack_group = source_ack_group,
        .recurrent = recurrent,
    };
    output->predecessors = destination_core->output_predecessors[destination_output];
    ++output->predecessor_count;
    return true;
}

static bool successor_endpoint_equal(const NmcOutputSuccessorMappingSpec *successor,
                                     const NmcSuccessorEndpoint *endpoint)
{
    return successor->core_id == endpoint->core_id && successor->input_group == endpoint->input_group;
}

static bool find_successor_endpoint(const NmcSuccessorEndpoint *endpoints,
                                    size_t endpoint_count,
                                    const NmcOutputSuccessorMappingSpec *successor,
                                    size_t *endpoint_index)
{
    for (size_t i = 0u; i < endpoint_count; ++i) {
        if (successor_endpoint_equal(successor, &endpoints[i])) {
            *endpoint_index = i;
            return true;
        }
    }
    return false;
}

static bool collect_successor_endpoints(const NmcCoreMappingSpec *mapping,
                                        NmcSuccessorEndpoint *endpoints,
                                        size_t *endpoint_count)
{
    *endpoint_count = 0u;
    for (size_t output_index = 0u; output_index < mapping->output_group_count; ++output_index) {
        const NmcOutputGroupMappingSpec *spec = &mapping->output_groups[output_index];
        if (!valid_spec_array(spec->successors, spec->successor_count)) {
            return false;
        }
        for (size_t i = 0u; i < spec->successor_count; ++i) {
            size_t endpoint_index = 0u;
            if (!find_successor_endpoint(endpoints, *endpoint_count, &spec->successors[i], &endpoint_index)) {
                if (*endpoint_count >= NMC_MAX_ACK_GROUPS) {
                    return false;
                }
                endpoints[*endpoint_count] = (NmcSuccessorEndpoint){
                    .core_id = spec->successors[i].core_id,
                    .input_group = spec->successors[i].input_group,
                };
                ++*endpoint_count;
            }
        }
    }
    return true;
}

bool nmc_core_mapping_ack_group_for_successor(const NmcCoreMappingSpec *mapping,
                                              nmc_core_id_t successor_core_id,
                                              nmc_input_index_t successor_input_group,
                                              nmc_ack_index_t *ack_group)
{
    if (mapping == NULL || ack_group == NULL || !valid_spec_array(mapping->output_groups, mapping->output_group_count)) {
        return false;
    }

    NmcSuccessorEndpoint endpoints[NMC_MAX_ACK_GROUPS];
    size_t endpoint_count = 0u;
    if (!collect_successor_endpoints(mapping, endpoints, &endpoint_count)) {
        return false;
    }

    for (size_t i = 0u; i < endpoint_count; ++i) {
        if (endpoints[i].core_id == successor_core_id && endpoints[i].input_group == successor_input_group) {
            *ack_group = (nmc_ack_index_t)i;
            return true;
        }
    }
    return false;
}

static bool output_accumulator_start(const NmcOutputGroupMappingSpec *spec,
                                     size_t *next_accumulator_start,
                                     size_t *accumulator_start)
{
    if (spec->accumulator_start == NMC_AUTO_ACCUMULATOR_OFFSET) {
        if (*next_accumulator_start > NMC_ACCUMULATOR_MEMORY_SIZE ||
            spec->width > NMC_ACCUMULATOR_MEMORY_SIZE - *next_accumulator_start) {
            return false;
        }
        *accumulator_start = *next_accumulator_start;
    } else {
        if (spec->accumulator_start > NMC_ACCUMULATOR_MEMORY_SIZE ||
            spec->width > NMC_ACCUMULATOR_MEMORY_SIZE - spec->accumulator_start) {
            return false;
        }
        *accumulator_start = spec->accumulator_start;
    }

    const size_t accumulator_end = *accumulator_start + spec->width;
    if (*next_accumulator_start < accumulator_end) {
        *next_accumulator_start = accumulator_end;
    }
    return true;
}

static bool valid_mapping_shape(const NmcCoreMappingSpec *mapping)
{
    if (!valid_spec_array(mapping->input_groups, mapping->input_group_count) ||
        !valid_spec_array(mapping->output_groups, mapping->output_group_count) ||
        mapping->input_group_count > NMC_MAX_INPUT_GROUPS ||
        mapping->output_group_count > NMC_MAX_OUTPUT_GROUPS) {
        return false;
    }

    for (size_t input_index = 0u; input_index < mapping->input_group_count; ++input_index) {
        if (!nmc_core_valid_width(mapping->input_groups[input_index].width)) {
            return false;
        }
    }

    for (size_t output_index = 0u; output_index < mapping->output_group_count; ++output_index) {
        const NmcOutputGroupMappingSpec *spec = &mapping->output_groups[output_index];
        if (!valid_spec_array(spec->inputs, spec->input_count) ||
            !valid_spec_array(spec->successors, spec->successor_count) ||
            !valid_spec_array(spec->predecessors, spec->predecessor_count) ||
            !nmc_core_valid_width(spec->width)) {
            return false;
        }
        for (size_t i = 0u; i < spec->input_count; ++i) {
            if ((size_t)spec->inputs[i].input_group >= mapping->input_group_count) {
                return false;
            }
        }
        for (size_t i = 0u; i < spec->successor_count; ++i) {
            if ((size_t)spec->successors[i].input_group >= NMC_MAX_INPUT_GROUPS) {
                return false;
            }
            for (size_t j = i + 1u; j < spec->successor_count; ++j) {
                if (spec->successors[i].core_id == spec->successors[j].core_id &&
                    spec->successors[i].input_group == spec->successors[j].input_group) {
                    return false;
                }
            }
        }
        for (size_t i = 0u; i < spec->predecessor_count; ++i) {
            if ((size_t)spec->predecessors[i].ack_group >= NMC_MAX_ACK_GROUPS) {
                return false;
            }
        }
    }
    return true;
}

static bool configure_input_groups(NmcCore *core, const NmcCoreMappingSpec *mapping)
{
    for (size_t i = 0u; i < mapping->input_group_count; ++i) {
        if (!nmc_core_add_input_group(core)) {
            return false;
        }
    }
    return true;
}

static bool configure_output_groups(NmcCore *core, const NmcCoreMappingSpec *mapping)
{
    size_t next_accumulator_start = 0u;
    for (size_t output_index = 0u; output_index < mapping->output_group_count; ++output_index) {
        const NmcOutputGroupMappingSpec *spec = &mapping->output_groups[output_index];
        size_t accumulator_start = 0u;
        if (!output_accumulator_start(spec, &next_accumulator_start, &accumulator_start) ||
            !nmc_core_add_output_group(core, spec->width, spec->thresholds) ||
            !nmc_core_set_output_accumulator_lut_start(core, (nmc_output_index_t)output_index, accumulator_start)) {
            return false;
        }
    }
    return true;
}

static bool configure_ack_groups(NmcCore *core, const NmcCoreMappingSpec *mapping)
{
    NmcSuccessorEndpoint endpoints[NMC_MAX_ACK_GROUPS];
    size_t endpoint_count = 0u;
    if (!collect_successor_endpoints(mapping, endpoints, &endpoint_count)) {
        return false;
    }
    for (size_t i = 0u; i < endpoint_count; ++i) {
        if (!nmc_core_add_ack_group(core)) {
            return false;
        }
    }
    return true;
}

static bool configure_output_routes(NmcCore *core, const NmcCoreMappingSpec *mapping)
{
    for (size_t output_index = 0u; output_index < mapping->output_group_count; ++output_index) {
        const NmcOutputGroupMappingSpec *spec = &mapping->output_groups[output_index];
        const size_t successor_start = core->output_route_lut_count;

        for (size_t i = 0u; i < spec->successor_count; ++i) {
            if (!nmc_core_add_output_successor_lut_entry(core,
                                                         spec->successors[i].core_id,
                                                         spec->successors[i].input_group)) {
                return false;
            }
        }

        const size_t predecessor_start = core->output_route_lut_count;
        for (size_t i = 0u; i < spec->predecessor_count; ++i) {
            const bool added = spec->predecessors[i].recurrent ?
                nmc_core_add_recurrent_output_predecessor_lut_entry(core,
                                                                    spec->predecessors[i].core_id,
                                                                    spec->predecessors[i].ack_group) :
                nmc_core_add_output_predecessor_lut_entry(core,
                                                          spec->predecessors[i].core_id,
                                                          spec->predecessors[i].ack_group);
            if (!added) {
                return false;
            }
        }

        if (!nmc_core_set_output_lut_starts(core, (nmc_output_index_t)output_index, successor_start, predecessor_start)) {
            return false;
        }
    }

    const size_t terminal_start = core->output_route_lut_count;
    return nmc_core_set_output_lut_starts(core, (nmc_output_index_t)mapping->output_group_count, terminal_start, terminal_start);
}

static bool configure_ack_luts(NmcCore *core, const NmcCoreMappingSpec *mapping)
{
    NmcSuccessorEndpoint endpoints[NMC_MAX_ACK_GROUPS];
    size_t endpoint_count = 0u;
    if (!collect_successor_endpoints(mapping, endpoints, &endpoint_count) || endpoint_count != core->ack_group_count) {
        return false;
    }

    for (size_t ack_index = 0u; ack_index < endpoint_count; ++ack_index) {
        const size_t start = core->ack_output_pair_lut_count;
        for (size_t output_index = 0u; output_index < mapping->output_group_count; ++output_index) {
            const NmcOutputGroupMappingSpec *spec = &mapping->output_groups[output_index];
            for (size_t i = 0u; i < spec->successor_count; ++i) {
                if (successor_endpoint_equal(&spec->successors[i], &endpoints[ack_index]) &&
                    !nmc_core_add_ack_output_pair_lut_entry(core, (nmc_output_index_t)output_index)) {
                    return false;
                }
            }
        }
        if (!nmc_core_set_ack_lut_start(core, (nmc_ack_index_t)ack_index, start)) {
            return false;
        }
    }

    return nmc_core_set_ack_lut_start(core,
                                      (nmc_ack_index_t)core->ack_group_count,
                                      core->ack_output_pair_lut_count);
}

static bool input_connection_weight_offset(const NmcCore *core,
                                           const NmcCoreMappingSpec *mapping,
                                           nmc_output_index_t output_index,
                                           const NmcInputConnectionMappingSpec *connection,
                                           size_t *next_weight_offset,
                                           size_t *weight_offset)
{
    if (!nmc_core_valid_output_index(core, output_index) ||
        (size_t)connection->input_group >= mapping->input_group_count) {
        return false;
    }

    const size_t output_width = core->output_groups[output_index].route_lut.bitmap_width;
    const size_t input_width = mapping->input_groups[connection->input_group].width;
    const size_t weight_count = output_width * input_width;
    if (connection->weight_offset == NMC_AUTO_WEIGHT_OFFSET) {
        if (*next_weight_offset > NMC_WEIGHT_MEMORY_SIZE || weight_count > NMC_WEIGHT_MEMORY_SIZE - *next_weight_offset) {
            return false;
        }
        *weight_offset = *next_weight_offset;
    } else {
        if (connection->weight_offset > NMC_WEIGHT_MEMORY_SIZE || weight_count > NMC_WEIGHT_MEMORY_SIZE - connection->weight_offset) {
            return false;
        }
        *weight_offset = connection->weight_offset;
    }

    const size_t weight_end = *weight_offset + weight_count;
    if (*next_weight_offset < weight_end) {
        *next_weight_offset = weight_end;
    }
    return true;
}

static void copy_input_connection_weights(NmcCore *core,
                                          const NmcInputConnectionMappingSpec *connection,
                                          size_t weight_offset,
                                          size_t weight_count)
{
    if (connection->weights != NULL) {
        memcpy(&core->weights[weight_offset], connection->weights, weight_count * sizeof(core->weights[0]));
    }
}

static bool configure_input_luts(NmcCore *core, const NmcCoreMappingSpec *mapping)
{
    size_t next_weight_offset = 0u;
    for (size_t input_index = 0u; input_index < core->input_group_count; ++input_index) {
        const size_t start = core->input_output_pair_lut_count;
        for (size_t output_index = 0u; output_index < mapping->output_group_count; ++output_index) {
            const NmcOutputGroupMappingSpec *spec = &mapping->output_groups[output_index];
            for (size_t i = 0u; i < spec->input_count; ++i) {
                const NmcInputConnectionMappingSpec *connection = &spec->inputs[i];
                if ((size_t)connection->input_group == input_index) {
                    size_t weight_offset = 0u;
                    const size_t output_width = core->output_groups[output_index].route_lut.bitmap_width;
                    const size_t input_width = mapping->input_groups[input_index].width;
                    const size_t weight_count = output_width * input_width;
                    if (!input_connection_weight_offset(core,
                                                        mapping,
                                                        (nmc_output_index_t)output_index,
                                                        connection,
                                                        &next_weight_offset,
                                                        &weight_offset)) {
                        return false;
                    }
                    copy_input_connection_weights(core, connection, weight_offset, weight_count);
                    const bool added = connection->recurrent ?
                        nmc_core_add_recurrent_input_output_pair_lut_entry(core,
                                                                          (nmc_output_index_t)output_index,
                                                                          weight_offset) :
                        nmc_core_add_input_output_pair_lut_entry(core,
                                                                (nmc_output_index_t)output_index,
                                                                weight_offset);
                    if (!added) {
                        return false;
                    }
                }
            }
        }
        if (!nmc_core_set_input_lut_start(core, (nmc_input_index_t)input_index, start)) {
            return false;
        }
    }

    return nmc_core_set_input_lut_start(core,
                                        (nmc_input_index_t)core->input_group_count,
                                        core->input_output_pair_lut_count);
}

static bool valid_network_mapping_shape(const NmcNetworkMappingSpec *network)
{
    if (network == NULL ||
        !valid_spec_array(network->groups, network->group_count) ||
        !valid_spec_array(network->inputs, network->input_count) ||
        !valid_spec_array(network->connections, network->connection_count) ||
        network->group_count > NMC_MAX_NETWORK_GROUPS ||
        network->input_count > NMC_MAX_NETWORK_INPUTS ||
        network->connection_count > NMC_MAX_NETWORK_CONNECTIONS) {
        return false;
    }

    for (size_t i = 0u; i < network->group_count; ++i) {
        if (!nmc_core_valid_width(network->groups[i].width)) {
            return false;
        }
        for (size_t j = i + 1u; j < network->group_count; ++j) {
            if (network->groups[i].group_id == network->groups[j].group_id) {
                return false;
            }
        }
    }

    for (size_t i = 0u; i < network->input_count; ++i) {
        size_t destination_index = 0u;
        if (!nmc_core_valid_width(network->inputs[i].width) ||
            !network_find_group(network, network->inputs[i].destination_group, &destination_index)) {
            return false;
        }
        for (size_t j = i + 1u; j < network->input_count; ++j) {
            if (network->inputs[i].input_id == network->inputs[j].input_id) {
                if (network->inputs[i].width != network->inputs[j].width) {
                    return false;
                }
                if (network->inputs[i].destination_group == network->inputs[j].destination_group) {
                    return false;
                }
            }
        }
    }

    for (size_t i = 0u; i < network->connection_count; ++i) {
        size_t source_index = 0u;
        size_t destination_index = 0u;
        if (!network_find_group(network, network->connections[i].source_group, &source_index) ||
            !network_find_group(network, network->connections[i].destination_group, &destination_index)) {
            return false;
        }
        for (size_t j = i + 1u; j < network->connection_count; ++j) {
            if (network->connections[i].source_group == network->connections[j].source_group &&
                network->connections[i].destination_group == network->connections[j].destination_group) {
                return false;
            }
        }
    }
    return true;
}

static bool add_generated_output_group(NmcGeneratedMappings *generated, const NmcNetworkGroupMappingSpec *group)
{
    NmcGeneratedCoreMapping *core = generated_get_or_add_core(generated, group->core_id);
    if (core == NULL || core->mapping.output_group_count >= NMC_MAX_OUTPUT_GROUPS) {
        return false;
    }

    const size_t output_index = core->mapping.output_group_count;
    core->output_group_ids[output_index] = group->group_id;
    core->output_groups[output_index] = (NmcOutputGroupMappingSpec){
        .width = group->width,
        .thresholds = group->thresholds,
        .accumulator_start = NMC_AUTO_ACCUMULATOR_OFFSET,
        .inputs = core->output_inputs[output_index],
        .input_count = 0u,
        .successors = core->output_successors[output_index],
        .successor_count = 0u,
        .predecessors = core->output_predecessors[output_index],
        .predecessor_count = 0u,
    };
    ++core->mapping.output_group_count;
    return true;
}

bool nmc_generate_core_mappings(const NmcNetworkMappingSpec *network, NmcGeneratedMappings *generated)
{
    if (generated == NULL || !valid_network_mapping_shape(network)) {
        return false;
    }

    memset(generated, 0, sizeof(*generated));

    for (size_t i = 0u; i < network->group_count; ++i) {
        if (!add_generated_output_group(generated, &network->groups[i])) {
            return false;
        }
    }

    for (size_t i = 0u; i < network->input_count; ++i) {
        NmcGeneratedGroupLocation destination;
        nmc_input_index_t input_group = NMC_INVALID_INDEX;
        if (!generated_find_group_location(generated, network->inputs[i].destination_group, &destination) ||
            !add_generated_external_input_connection(destination.core,
                                                     destination.output_index,
                                                     network->inputs[i].input_id,
                                                     network->inputs[i].width,
                                                     network->inputs[i].destination_group,
                                                     network->inputs[i].weights,
                                                     &input_group)) {
            return false;
        }
    }

    nmc_input_index_t connection_input_groups[NMC_MAX_NETWORK_CONNECTIONS];
    for (size_t i = 0u; i < NMC_MAX_NETWORK_CONNECTIONS; ++i) {
        connection_input_groups[i] = NMC_INVALID_INDEX;
    }

    for (size_t i = 0u; i < network->connection_count; ++i) {
        size_t source_group_index = 0u;
        NmcGeneratedGroupLocation source;
        NmcGeneratedGroupLocation destination;
        nmc_input_index_t destination_input = NMC_INVALID_INDEX;
        const bool recurrent = network_connection_is_recurrent(network, &network->connections[i]);
        if (!network_find_group(network, network->connections[i].source_group, &source_group_index) ||
            !generated_find_group_location(generated, network->connections[i].source_group, &source) ||
            !generated_find_group_location(generated, network->connections[i].destination_group, &destination) ||
            !add_generated_input_connection(destination.core,
                                            destination.output_index,
                                            network->groups[source_group_index].width,
                                            network->connections[i].source_group,
                                            network->connections[i].destination_group,
                                            NMC_AUTO_WEIGHT_OFFSET,
                                            network->connections[i].weights,
                                            recurrent,
                                            &destination_input) ||
            !add_generated_successor(source.core, source.output_index, destination.core->core_id, destination_input)) {
            return false;
        }
        connection_input_groups[i] = destination_input;
    }

    for (size_t i = 0u; i < network->connection_count; ++i) {
        NmcGeneratedGroupLocation source;
        NmcGeneratedGroupLocation destination;
        nmc_ack_index_t source_ack_group = NMC_INVALID_INDEX;
        const bool recurrent = network_connection_is_recurrent(network, &network->connections[i]);
        if (!generated_find_group_location(generated, network->connections[i].source_group, &source) ||
            !generated_find_group_location(generated, network->connections[i].destination_group, &destination) ||
            !nmc_core_mapping_ack_group_for_successor(&source.core->mapping,
                                                      destination.core->core_id,
                                                      connection_input_groups[i],
                                                      &source_ack_group) ||
            !add_generated_predecessor(destination.core,
                                       destination.output_index,
                                       source.core->core_id,
                                       source_ack_group,
                                       recurrent)) {
            return false;
        }
    }

    for (size_t i = 0u; i < generated->core_count; ++i) {
        if (!valid_mapping_shape(&generated->cores[i].mapping)) {
            return false;
        }
    }
    return true;
}

bool nmc_core_configure_mapping(NmcCore *core, nmc_core_id_t core_id, const NmcCoreMappingSpec *mapping)
{
    if (core == NULL || mapping == NULL || !valid_mapping_shape(mapping)) {
        return false;
    }

    nmc_core_init(core, core_id);
    return configure_input_groups(core, mapping) &&
           configure_output_groups(core, mapping) &&
           configure_input_luts(core, mapping) &&
           configure_ack_groups(core, mapping) &&
           configure_output_routes(core, mapping) &&
           configure_ack_luts(core, mapping);
}
