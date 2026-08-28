/*
 * Copyright (c) 2022 Mariotaku <https://github.com/mariotaku>.
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ihs_thread.h"
#include "ihs_timer.h"
#include "packet.h"

typedef struct IHS_Session IHS_Session;
typedef struct IHS_RetransmissionPending IHS_RetransmissionPending;

typedef bool (*IHS_RetransmissionSendFunction)(IHS_SessionPacket *packet, void *context);

typedef struct IHS_RetransmissionStats {
    uint64_t tracked;
    uint64_t acknowledged;
    uint64_t superseded;
    uint64_t giveUps;
    uint64_t nacks;
    uint64_t retries;
    uint64_t sendFailures;
    uint32_t outstanding;
    uint64_t oldestOutstandingMs;
    uint32_t oldestChannelId;
    uint32_t oldestPacketId;
    int32_t oldestFragmentId;
    uint32_t oldestRetryCount;
    uint64_t maxAckLatencyMs;
} IHS_RetransmissionStats;

/**
 * One session-owned reliability state machine. A packet is tracked before its
 * first send is queued and remains here until the peer acknowledges the exact
 * (channel, packet, fragment) identity, its owner retires a superseded packet
 * after bounded gap filling, the give-up window (3 s) closes on an unacked
 * packet — the peer's decrypt-sequence resync makes later retransmissions
 * stale replays — or the session is destroyed.
 */
typedef struct IHS_SessionRetransmission {
    IHS_Session *session;
    IHS_Mutex *lock;
    IHS_RetransmissionPending *head;
    IHS_TimerTask *timer;
    IHS_RetransmissionStats stats;
} IHS_SessionRetransmission;

void IHS_RetransmissionInit(IHS_SessionRetransmission *retransmission, IHS_Session *session);

void IHS_RetransmissionDeinit(IHS_SessionRetransmission *retransmission);

bool IHS_RetransmissionIsTracked(const IHS_SessionRetransmission *retransmission,
                                 IHS_SessionChannelId channelId, uint16_t packetId,
                                 int16_t fragmentId);

bool IHS_RetransmissionTrack(IHS_SessionRetransmission *retransmission,
                             const IHS_SessionPacket *packet, uint64_t nowMs);

bool IHS_RetransmissionAcknowledge(IHS_SessionRetransmission *retransmission,
                                   IHS_SessionChannelId channelId, uint16_t packetId,
                                   int16_t fragmentId, uint64_t nowMs);

/** Mark an exact packet as superseded by a newer full-state message. It remains
 * eligible for a small, bounded number of retries before being retired. */
bool IHS_RetransmissionSupersede(IHS_SessionRetransmission *retransmission,
                                 IHS_SessionChannelId channelId, uint16_t packetId,
                                 int16_t fragmentId);

bool IHS_RetransmissionNack(IHS_SessionRetransmission *retransmission,
                            IHS_SessionChannelId channelId, uint16_t packetId,
                            int16_t fragmentId, uint64_t nowMs);

void IHS_RetransmissionNoteInitialSend(IHS_SessionRetransmission *retransmission,
                                       const IHS_SessionPacketHeader *header, bool sent,
                                       uint64_t nowMs);

size_t IHS_RetransmissionProcessAt(IHS_SessionRetransmission *retransmission, uint64_t nowMs,
                                   IHS_RetransmissionSendFunction send, void *context);

void IHS_RetransmissionGetStats(IHS_SessionRetransmission *retransmission,
                                IHS_RetransmissionStats *stats, uint64_t nowMs);
