#include "nmc/internal/core.h"

#include <string.h>

static void install_builtin_activation_programs(NmcCore *core)
{
    static const NmcActivationInstruction if_immediate[] = {
        {.opcode = NMC_ACT_OP_LD_ACC, .dst = 0u},
        {.opcode = NMC_ACT_OP_LD_IMM, .dst = 1u, .immediate = NMC_ACTIVATION_THRESHOLD_IMMEDIATE},
        {.opcode = NMC_ACT_OP_CMP_GE, .dst = 0u, .src0 = 0u, .src1 = 1u},
        {.opcode = NMC_ACT_OP_EMIT_PRED, .src0 = 0u},
        {.opcode = NMC_ACT_OP_LD_IMM, .dst = 2u, .immediate = NMC_ACTIVATION_RESET_IMMEDIATE},
        {.opcode = NMC_ACT_OP_ST_ACC, .src0 = 2u},
        {.opcode = NMC_ACT_OP_END},
    };
    static const NmcActivationInstruction if_sram_threshold[] = {
        {.opcode = NMC_ACT_OP_LD_ACC, .dst = 0u},
        {.opcode = NMC_ACT_OP_LD_WORD, .dst = 1u, .range = NMC_ACTIVATION_THRESHOLD_RANGE},
        {.opcode = NMC_ACT_OP_CMP_GE, .dst = 0u, .src0 = 0u, .src1 = 1u},
        {.opcode = NMC_ACT_OP_EMIT_PRED, .src0 = 0u},
        {.opcode = NMC_ACT_OP_LD_IMM, .dst = 2u, .immediate = NMC_ACTIVATION_RESET_IMMEDIATE},
        {.opcode = NMC_ACT_OP_ST_ACC, .src0 = 2u},
        {.opcode = NMC_ACT_OP_END},
    };

    (void)nmc_core_add_activation_program(core,
                                          if_immediate,
                                          sizeof(if_immediate) / sizeof(if_immediate[0]),
                                          NMC_MAX_ACTIVATION_STEPS,
                                          NULL);
    (void)nmc_core_add_activation_program(core,
                                          if_sram_threshold,
                                          sizeof(if_sram_threshold) / sizeof(if_sram_threshold[0]),
                                          NMC_MAX_ACTIVATION_STEPS,
                                          NULL);
}

void nmc_core_init(NmcCore *core, nmc_core_id_t core_id)
{
    memset(core, 0, sizeof(*core));
    core->core_id = core_id;
    install_builtin_activation_programs(core);
}
