#include "nmc/internal/core.h"

#include <string.h>

void nmc_core_init(NmcCore *core, nmc_core_id_t core_id)
{
    memset(core, 0, sizeof(*core));
    core->core_id = core_id;
}