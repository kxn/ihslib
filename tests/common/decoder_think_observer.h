/* Input release precedes OnThink and cannot prove SDK/assembly completion.
 * Forward the real consumer step, observing completion without changing its
 * class, return value, inputs or scheduling. processed is fixture-owned. */
#pragma once
#include "session/channels/ch_data.h"
#include <stdatomic.h>

static atomic_uint decoderInputsConsumed;
uint32_t __real_IHS_SessionChannelDataThink(IHS_SessionChannel *);
uint32_t __wrap_IHS_SessionChannelDataThink(IHS_SessionChannel *ch) {
    uint32_t timeout = __real_IHS_SessionChannelDataThink(ch);
    if (ch->type == IHS_SessionChannelTypeDataVideo && timeout != 1)
        atomic_store(&processed, atomic_load(&decoderInputsConsumed));
    return timeout;
}
