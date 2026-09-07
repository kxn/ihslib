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
    uint64_t hidAcknowledged; /* HID transport packets, not host-applied reports */
    uint32_t hidPending, hidInFlight;
    int32_t hidOldestInFlightPacketId;
} IHS_RetransmissionStats;

/** Reliable packets remain tracked until peer confirmation or session teardown.
 * ACK is cumulative; extended NACK includes cumulative and selective receipt.
 * New reports cannot supersede unacknowledged ciphertext. */
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

/** Compatibility lookup only: superseding cannot retire reliable ciphertext. */
bool IHS_RetransmissionSupersede(IHS_SessionRetransmission *retransmission,
                                 IHS_SessionChannelId channelId, uint16_t packetId,
                                 int16_t fragmentId);

bool IHS_RetransmissionNack(IHS_SessionRetransmission *retransmission,
                            IHS_SessionChannelId channelId, uint16_t packetId,
                            int16_t fragmentId, uint64_t nowMs);

/* AcknowledgeThrough uses an exclusive boundary; pass confirmed+1.
 * Acknowledge/Nack accept INT16_MIN to match an ID regardless of fragment. */
/* Force retransmit (set due immediately) of every tracked pending packet on
 * the channel strictly below packetId. Used for the simple (header-only) NACK. */
size_t IHS_RetransmissionNackAllThrough(IHS_SessionRetransmission *retransmission,
                                        IHS_SessionChannelId channelId, uint16_t packetId,
                                        uint64_t nowMs);

size_t IHS_RetransmissionAcknowledgeThrough(IHS_SessionRetransmission *retransmission,
                                            IHS_SessionChannelId channelId, uint16_t packetId,
                                            uint64_t nowMs);

void IHS_RetransmissionNoteInitialSend(IHS_SessionRetransmission *retransmission,
                                       const IHS_SessionPacketHeader *header, bool sent,
                                       uint64_t nowMs);

size_t IHS_RetransmissionProcessAt(IHS_SessionRetransmission *retransmission, uint64_t nowMs,
                                   IHS_RetransmissionSendFunction send, void *context);

void IHS_RetransmissionGetStats(IHS_SessionRetransmission *retransmission,
                                IHS_RetransmissionStats *stats, uint64_t nowMs);

size_t IHS_RetransmissionNackBefore(IHS_SessionRetransmission *r, IHS_SessionChannelId channel,
                                    uint16_t id, bool below, uint32_t cutoff, uint64_t nowMs);
