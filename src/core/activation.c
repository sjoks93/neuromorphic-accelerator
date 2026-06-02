#include "nmc/internal/core.h"

#include <limits.h>
#include <string.h>

static int32_t saturate_i64_to_i32(int64_t value)
{
    if (value > INT32_MAX) {
        return INT32_MAX;
    }
    if (value < INT32_MIN) {
        return INT32_MIN;
    }
    return (int32_t)value;
}

static bool activation_valid_register(uint8_t reg)
{
    return reg < NMC_MAX_ACTIVATION_REGISTERS;
}

static bool activation_load_word(const NmcCore *core,
                                 const NmcOutputGroup *group,
                                 const NmcActivationInstruction *instruction,
                                 size_t lane,
                                 int32_t *value)
{
    if (instruction->range >= NMC_MAX_ACTIVATION_SRAM_RANGES) {
        return false;
    }

    const NmcActivationSramRange *range = &group->activation_sram_ranges[instruction->range];
    if (!range->valid || lane >= range->count) {
        return false;
    }

    return nmc_core_memory_read_activation_word(core, range->start + lane, value);
}

static bool activation_store_word(NmcCore *core,
                                  const NmcOutputGroup *group,
                                  const NmcActivationInstruction *instruction,
                                  size_t lane,
                                  int32_t value)
{
    if (instruction->range >= NMC_MAX_ACTIVATION_SRAM_RANGES) {
        return false;
    }

    const NmcActivationSramRange *range = &group->activation_sram_ranges[instruction->range];
    if (!range->valid || lane >= range->count) {
        return false;
    }

    return nmc_core_memory_write_activation_word(core, range->start + lane, value);
}

static bool activation_execute_lane(NmcCore *core,
                                    nmc_output_index_t output_index,
                                    size_t accumulator_start,
                                    size_t lane,
                                    uint8_t *payload,
                                    uint32_t *instruction_count,
                                    uint64_t *cycle_count)
{
    NmcOutputGroup *group = &core->output_groups[output_index];
    if (group->activation_program_index >= core->activation_program_count) {
        return false;
    }

    const NmcActivationProgramDescriptor *program = &core->activation_programs[group->activation_program_index];
    if (!program->valid ||
        program->start > core->activation_instruction_count ||
        program->length > core->activation_instruction_count - program->start ||
        program->max_steps == 0u) {
        return false;
    }

    int32_t registers[NMC_MAX_ACTIVATION_REGISTERS] = {0};
    bool predicates[NMC_MAX_ACTIVATION_REGISTERS] = {false};
    bool emitted = false;

    for (uint32_t step = 0u; step < program->max_steps; ++step) {
        if (step >= program->length) {
            return false;
        }

        const NmcActivationInstruction *instruction = &core->activation_instructions[program->start + step];
        ++*instruction_count;
        ++*cycle_count;

        switch (instruction->opcode) {
        case NMC_ACT_OP_END:
            return emitted;

        case NMC_ACT_OP_LD_ACC:
            if (!activation_valid_register(instruction->dst) ||
                !nmc_core_memory_read_accumulator(core,
                                                  accumulator_start,
                                                  lane,
                                                  NMC_MEMORY_OUTPUT_TO_ACCUMULATOR_MUX,
                                                  &registers[instruction->dst])) {
                return false;
            }
            break;

        case NMC_ACT_OP_ST_ACC:
            if (!activation_valid_register(instruction->src0) ||
                !nmc_core_memory_write_accumulator(core, accumulator_start, lane, registers[instruction->src0])) {
                return false;
            }
            break;

        case NMC_ACT_OP_LD_WORD:
            if (!activation_valid_register(instruction->dst) ||
                !activation_load_word(core, group, instruction, lane, &registers[instruction->dst])) {
                return false;
            }
            break;

        case NMC_ACT_OP_ST_WORD:
            if (!activation_valid_register(instruction->src0) ||
                !activation_store_word(core, group, instruction, lane, registers[instruction->src0])) {
                return false;
            }
            break;

        case NMC_ACT_OP_LD_IMM:
            if (!activation_valid_register(instruction->dst) || instruction->immediate >= NMC_MAX_ACTIVATION_IMMEDIATES) {
                return false;
            }
            registers[instruction->dst] = group->activation_immediates[instruction->immediate];
            break;

        case NMC_ACT_OP_ADD:
            if (!activation_valid_register(instruction->dst) ||
                !activation_valid_register(instruction->src0) ||
                !activation_valid_register(instruction->src1)) {
                return false;
            }
            registers[instruction->dst] = saturate_i64_to_i32((int64_t)registers[instruction->src0] + registers[instruction->src1]);
            break;

        case NMC_ACT_OP_SUB:
            if (!activation_valid_register(instruction->dst) ||
                !activation_valid_register(instruction->src0) ||
                !activation_valid_register(instruction->src1)) {
                return false;
            }
            registers[instruction->dst] = saturate_i64_to_i32((int64_t)registers[instruction->src0] - registers[instruction->src1]);
            break;

        case NMC_ACT_OP_MUL:
            if (!activation_valid_register(instruction->dst) ||
                !activation_valid_register(instruction->src0) ||
                !activation_valid_register(instruction->src1) ||
                instruction->shift >= NMC_ACCUMULATOR_BITS) {
                return false;
            }
            *cycle_count += NMC_ACTIVATION_MUL_LATENCY - 1u;
            registers[instruction->dst] = saturate_i64_to_i32(((int64_t)registers[instruction->src0] * registers[instruction->src1]) >> instruction->shift);
            break;

        case NMC_ACT_OP_MAC:
            if (!activation_valid_register(instruction->dst) ||
                !activation_valid_register(instruction->src0) ||
                !activation_valid_register(instruction->src1) ||
                instruction->shift >= NMC_ACCUMULATOR_BITS) {
                return false;
            }
            *cycle_count += NMC_ACTIVATION_MUL_LATENCY - 1u;
            registers[instruction->dst] = saturate_i64_to_i32((int64_t)registers[instruction->dst] +
                                                              (((int64_t)registers[instruction->src0] * registers[instruction->src1]) >> instruction->shift));
            break;

        case NMC_ACT_OP_CMP_GE:
            if (!activation_valid_register(instruction->dst) ||
                !activation_valid_register(instruction->src0) ||
                !activation_valid_register(instruction->src1)) {
                return false;
            }
            predicates[instruction->dst] = registers[instruction->src0] >= registers[instruction->src1];
            break;

        case NMC_ACT_OP_EMIT_PRED:
            if (!activation_valid_register(instruction->src0) || emitted) {
                return false;
            }
            if (predicates[instruction->src0]) {
                nmc_core_payload_set_bit(payload, lane);
            }
            emitted = true;
            break;

        default:
            return false;
        }
    }

    return false;
}

bool nmc_core_execute_activation_program(NmcCore *core,
                                         nmc_output_index_t output_index,
                                         size_t accumulator_start,
                                         uint8_t *payload)
{
    if (!nmc_core_valid_output_index(core, output_index) || payload == NULL) {
        return false;
    }

    NmcOutputGroup *group = &core->output_groups[output_index];
    const nmc_tile_width_t width = group->route_lut.bitmap_width;
    memset(payload, 0, NMC_MAX_GROUP_BYTES);

    uint32_t instruction_count = 0u;
    uint64_t cycle_count = 0u;
    for (nmc_tile_width_t lane = 0u; lane < width; ++lane) {
        if (!activation_execute_lane(core,
                                     output_index,
                                     accumulator_start,
                                     lane,
                                     payload,
                                     &instruction_count,
                                     &cycle_count)) {
            ++core->activation_fault_count;
            return false;
        }
    }

    const uint64_t vector_cycles = (cycle_count + NMC_OUTPUT_PARALLELISM - 1u) / NMC_OUTPUT_PARALLELISM;
    core->last_activation_instruction_count = instruction_count;
    core->last_activation_cycles = vector_cycles;
    core->last_activation_output_width = width;
    core->total_activation_cycles += vector_cycles;
    return true;
}
