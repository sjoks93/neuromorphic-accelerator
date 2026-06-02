#include "nmc/internal/core.h"

bool nmc_core_process_ack(NmcCore *core, const NmcAckMessage *ack)
{
    /* The router has selected this core; the local destination group field is the ACK index. */
    if (ack->destination_count != 1u) {
        return false;
    }

    const nmc_ack_index_t ack_index = (nmc_ack_index_t)ack->destinations[0].group_index;
    if (!nmc_core_valid_ack_index(core, ack_index)) {
        return false;
    }

    /* The following ACK-group entry provides the exclusive end of this range. */
    const NmcAckGroup *ack_group = &core->ack_groups[ack_index];
    const NmcAckGroup *next_ack_group = &core->ack_groups[ack_index + 1u];
    if (!ack_group->lut.valid || !next_ack_group->lut.valid || ack_group->lut.start > next_ack_group->lut.start) {
        return false;
    }

    bool matched = false;
    for (size_t i = ack_group->lut.start; i < next_ack_group->lut.start; ++i) {
        const NmcAckOutputPairLutEntry *pair_entry = &core->ack_output_pair_lut[i];
        if (!nmc_core_valid_output_index(core, pair_entry->output_index)) {
            return false;
        }

        NmcOutputGroup *output_group = &core->output_groups[pair_entry->output_index];
        if (output_group->ack_count == UINT32_MAX) {
            return false;
        }

        const size_t successor_count = nmc_core_output_group_successor_count(output_group);
        if (successor_count == 0u || output_group->ack_count >= successor_count) {
            return false;
        }

        /* Returning an ACK may unblock a previously complete output group. */
        ++output_group->ack_count;
        matched = true;
    }

    (void)nmc_core_flush_ready_outputs(core);
    return matched;
}
