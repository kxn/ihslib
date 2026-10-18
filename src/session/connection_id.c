/* Official client GetNextConnectionID 0x7fab70; process-wide nonzero IDs. */
#include "crypto.h"
#include "session_pri.h"

static atomic_uint nextConnectionId;

uint8_t IHS_SessionNextConnectionId(void) {
    /* GetNextConnectionID 0x7fab70: one process-wide nonzero random seed modulo
     * 255, then sequential 1..255 IDs. Entropy uses this library's provider. */
    unsigned current = atomic_load(&nextConnectionId);
    for (;;) {
        unsigned id = current;
        while (!id)
            id = (IHS_CryptoRandomUInt32() & 0x7fffffffu) % 255u;
        unsigned next = id == 255 ? 1 : id + 1;
        if (atomic_compare_exchange_strong(&nextConnectionId, &current, next))
            return (uint8_t)id;
    }
}
