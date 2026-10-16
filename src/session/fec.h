/* Steam type-11 receiver. Protocol evidence: nsteamlink reference §26, §29.6.
 * One active group, independent of the ordinary unreliable receive window.
 * Caller serializes Add/Destroy. Input packets remain owned by the caller;
 * a successful Add transfers an owned, complete payload into frame. */
#pragma once

#include "frame.h"

typedef struct IHS_FECReceiver IHS_FECReceiver;

/* Project memory bound, not an advertised Steam protocol maximum. */
#define IHS_FEC_MAX_PAYLOAD (2u * 1024u * 1024u)

IHS_FECReceiver *IHS_FECReceiverCreate(void);
void IHS_FECReceiverDestroy(IHS_FECReceiver *receiver);
bool IHS_FECReceiverAdd(IHS_FECReceiver *receiver, const IHS_SessionPacket *packet,
                        IHS_SessionFrame *frame);
