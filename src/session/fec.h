/* Steam type-11 receiver. Protocol evidence: nsteamlink reference §26, §29.6.
 * One active group, independent of the ordinary unreliable receive window.
 * Caller serializes Add/Destroy. Input packets remain owned by the caller;
 * a successful Add transfers an owned, complete payload into frame. */
#pragma once

#include "frame.h"

typedef struct IHS_FECReceiver IHS_FECReceiver;

/* Local DATA storage budget, not a wire or hardware maximum. The former 2MiB
 * logical-length cap already admitted ceil(2MiB/S)*S bytes, up to 2162655 at
 * S=65535. Bound that actual storage instead, so media envelope bytes do not
 * consume the encoded-image limit. Presence/parity/RS bounds remain separate.
 * The exhaustive metadata test preserves the old worst allocation domain. */
#define IHS_FEC_MAX_DATA_BYTES (((2u * 1024u * 1024u + UINT16_MAX - 1u) / UINT16_MAX) * UINT16_MAX)

IHS_FECReceiver *IHS_FECReceiverCreate(void);
void IHS_FECReceiverDestroy(IHS_FECReceiver *receiver);
bool IHS_FECReceiverAdd(IHS_FECReceiver *receiver, const IHS_SessionPacket *packet,
                        IHS_SessionFrame *frame);

/* Read under the same owner/lock as Add. Malformed/duplicate/incomplete input
 * remains a normal false; an actual allocation failure is a local error. */
typedef enum IHS_FECReceiveError {
    IHS_FECReceiveOK,
    IHS_FECReceiveNoMemory,
} IHS_FECReceiveError;
IHS_FECReceiveError IHS_FECReceiverLastError(const IHS_FECReceiver *receiver);
