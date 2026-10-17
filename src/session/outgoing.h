/* SPDX-License-Identifier: LGPL-3.0-or-later */
#pragma once
#include "packet.h"
#include <stdint.h>

typedef struct IHS_Session IHS_Session;
typedef struct IHS_OutgoingPacket {
    IHS_SessionPacket packet;
} IHS_OutgoingPacket;
typedef struct IHS_OutgoingWindow {
    IHS_OutgoingPacket **slots;
    uint32_t *created;
    unsigned capacity;
    uint16_t head, tail, cursor;
    bool initialized;
    uint32_t lastFeedback;
} IHS_OutgoingWindow;
typedef struct IHS_Outgoing {
    /* Cache/shaper fields use Base; factory/count/order/priority/IDs use
     * sendQueueMutex. Sender snapshots order before Socket I/O. No channel pointers
     * are retained: channel objects can stop while the connection still owns TX. */
    int32_t rate[2];
    uint32_t deadline[2];
    bool blocked, dirty;
    uint8_t count, rotation, order[32], creation[32];
    bool active[32];
    int32_t priority[32];
    uint16_t reliableId[32], unreliableId[32];
    IHS_OutgoingWindow unreliable[32];
} IHS_Outgoing;
void IHS_OutgoingInit(IHS_Session *, uint32_t now);
void IHS_OutgoingDeinit(IHS_Session *); /* Send worker and producers joined. */
void IHS_OutgoingRegister(IHS_Session *, uint8_t channel);
void IHS_OutgoingPriority(IHS_Session *, uint8_t channel, int32_t priority);
uint16_t IHS_OutgoingNextId(IHS_Session *, uint8_t channel, bool reliable);
/* Caller holds Base lock across CanSend -> transport -> Sent. */
bool IHS_OutgoingCanSend(IHS_Session *, unsigned type, uint32_t now);
void IHS_OutgoingSent(IHS_Session *, unsigned type, uint32_t stamp, uint32_t wireBytes);
/* Retain transfers the body on success. Packet IDs were reserved by the caller. */
bool IHS_OutgoingRetain(IHS_Session *, IHS_SessionPacket *, uint32_t created);
int IHS_OutgoingSendUnreliable(IHS_Session *, uint8_t channel, bool retry);
void IHS_OutgoingNack(IHS_Session *, const IHS_SessionPacket *);
void IHS_OutgoingUpdate(IHS_Session *, uint32_t now);
/* One factory/priority snapshot, shared by RX and TX scans. */
unsigned IHS_OutgoingSnapshot(IHS_Session *, uint8_t order[32], unsigned *rotation);
/* Sender actor wakes for producer work and also on a bounded clock wait. */
void IHS_SessionWakeSend(IHS_Session *);
int IHS_SessionSendPacketResult(IHS_Session *, IHS_SessionPacket *);

void IHS_OutgoingSort(IHS_Outgoing *);
