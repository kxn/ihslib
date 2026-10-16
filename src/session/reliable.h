/* SPDX-License-Identifier: LGPL-3.0-or-later */
#pragma once
#include "window.h"

typedef struct IHS_Session IHS_Session;
typedef struct IHS_ReliableChannel {
    IHS_SessionPacketsWindow *framePacketWindow;
    uint32_t peerTimestamp, peerReceiveTime, feedbackDeadline, lastNackTimestamp;
    uint16_t ackWatermark;
    bool feedbackDirty, ackPending, overflowed;
} IHS_ReliableChannel;

/* Protocol slots outlive consumer/decoder replacement. Factory uses queue mutex;
 * received state uses reliableLock; frame delegates run outside those locks. */
IHS_ReliableChannel *IHS_SessionReliableGet(IHS_Session *, uint8_t channel);
void IHS_SessionReliableReceived(IHS_Session *, IHS_SessionPacket *);
void IHS_SessionReliableUpdateChannel(IHS_Session *, uint8_t channel);
void IHS_SessionReliableUpdate(IHS_Session *);
void IHS_SessionReliableDeinit(IHS_Session *);

typedef enum IHS_ReliableDisposition {
    IHS_ReliableDiscarded,
    IHS_ReliableIgnored,
    IHS_ReliableControl,
    IHS_ReliablePending,
    IHS_ReliableData,
} IHS_ReliableDisposition;
IHS_ReliableDisposition IHS_SessionDeliverReliableFrame(IHS_Session *, IHS_SessionFrame *);
