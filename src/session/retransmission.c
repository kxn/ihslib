/*
 * Copyright (c) 2022 Mariotaku <https://github.com/mariotaku>.
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "retransmission.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "session_pri.h"

#define RETRANSMISSION_TICK_MS 5
#define RETRANSMISSION_INITIAL_MS 25
/* Official pacing: timeout = 1.25 x clamp(conn timeout estimate, 65..3276
 * (0.99..50.0 ms in 16.16s units)) -- libmain 0x7f8ac4 with the static
 * initializer at 0x7fe284 ([f47000+2436]=65, [+2440]=3276). The dynamic
 * estimate is not replicated; 62 ms matches the official ceiling. */
#define RETRANSMISSION_MAX_MS 62
#define RETRANSMISSION_SUPERSEDE_MIN_RETRIES 3
/* No give-up: the official client retransmits reliable packets until they are
 * ACKed or released by a peer NACK (libmain.so 0x7f8ac4 SendReliablePackets /
 * 0x7f94b8 HandleAckPacket / 0x7f95dc HandleNackPacket contain no retire
 * path). The host delivers encrypted frames strictly in order, so a delayed
 * retransmit keeps a valid sequence number and heals the hole. The former
 * 3 s give-up created permanent holes that wedged the host's ordered window
 * (~20 s apply stall). See docs/STEAMLINK_PROTOCOL_RE.md §9. */

struct IHS_RetransmissionPending {
    IHS_SessionPacket packet;
    uint64_t firstTrackedMs;
    uint64_t lastSendMs;
    uint64_t nextRetryMs;
    uint32_t retryCount;
    bool initialSent;
    bool nackPending;
    bool superseded;
    IHS_RetransmissionPending *next;
};

static bool PacketIdentityMatches(const IHS_SessionPacketHeader *header,
                                  IHS_SessionChannelId channelId, uint16_t packetId,
                                  int16_t fragmentId) {
    return header->channelId == channelId && header->packetId == packetId &&
           header->fragmentId == fragmentId;
}

static void PacketClone(IHS_SessionPacket *dest, const IHS_SessionPacket *source) {
    memset(dest, 0, sizeof(*dest));
    dest->header = source->header;
    dest->crc = source->crc;
    IHS_SessionPacketBodyInitialize(&dest->body, dest->header.hasCrc);
    IHS_BufferAppendMem(&dest->body, IHS_BufferPointer(&source->body), source->body.size);
}

static void PendingDestroy(IHS_RetransmissionPending *pending) {
    IHS_SessionPacketClear(&pending->packet, true);
    free(pending);
}

static uint64_t RetryDelayMs(uint32_t retryCount) {
    uint32_t shift = retryCount > 4 ? 4 : retryCount;
    uint64_t delay = (uint64_t) RETRANSMISSION_INITIAL_MS << shift;
    return delay < RETRANSMISSION_MAX_MS ? delay : RETRANSMISSION_MAX_MS;
}

static bool SendRetry(IHS_SessionPacket *packet, void *context) {
    return IHS_SessionSendPacket(context, packet);
}

static uint64_t RetransmissionTimerRun(int runCount, void *context) {
    (void) runCount;
    IHS_SessionRetransmission *retransmission = context;
    IHS_RetransmissionProcessAt(retransmission, IHS_TimerNow(), SendRetry,
                                retransmission->session);
    return RETRANSMISSION_TICK_MS;
}

static void RetransmissionTimerEnd(void *context) {
    IHS_SessionRetransmission *retransmission = context;
    retransmission->timer = NULL;
}

void IHS_RetransmissionInit(IHS_SessionRetransmission *retransmission, IHS_Session *session) {
    memset(retransmission, 0, sizeof(*retransmission));
    retransmission->session = session;
    retransmission->lock = IHS_MutexCreate();
    if (session != NULL && session->timers != NULL) {
        retransmission->timer = IHS_TimerTaskStart(session->timers, RetransmissionTimerRun,
                                                   RetransmissionTimerEnd,
                                                   RETRANSMISSION_TICK_MS, retransmission);
    }
}

void IHS_RetransmissionDeinit(IHS_SessionRetransmission *retransmission) {
    IHS_MutexLock(retransmission->lock);
    IHS_RetransmissionPending *pending = retransmission->head;
    retransmission->head = NULL;
    retransmission->stats.outstanding = 0;
    IHS_MutexUnlock(retransmission->lock);
    while (pending != NULL) {
        IHS_RetransmissionPending *next = pending->next;
        PendingDestroy(pending);
        pending = next;
    }
    IHS_MutexDestroy(retransmission->lock);
}

bool IHS_RetransmissionTrack(IHS_SessionRetransmission *retransmission,
                             const IHS_SessionPacket *packet, uint64_t nowMs) {
    assert(packet->body.data != NULL);
    assert(packet->body.offset == IHS_PACKET_HEADER_SIZE);

    IHS_RetransmissionPending *pending = calloc(1, sizeof(*pending));
    if (pending == NULL) {
        return false;
    }
    PacketClone(&pending->packet, packet);
    pending->firstTrackedMs = nowMs;
    /* Registration precedes queue admission so a fast ACK cannot race past us.
     * Do not arm retry here: the send queue may take longer than the retry delay,
     * which used to let a retransmission overtake the initial datagram. */
    pending->nextRetryMs = UINT64_MAX;

    IHS_MutexLock(retransmission->lock);
    for (IHS_RetransmissionPending *item = retransmission->head; item != NULL;
         item = item->next) {
        if (PacketIdentityMatches(&item->packet.header, packet->header.channelId,
                                  packet->header.packetId, packet->header.fragmentId)) {
            IHS_MutexUnlock(retransmission->lock);
            PendingDestroy(pending);
            return false;
        }
    }
    pending->next = retransmission->head;
    retransmission->head = pending;
    retransmission->stats.tracked++;
    retransmission->stats.outstanding++;
    IHS_MutexUnlock(retransmission->lock);
    return true;
}

bool IHS_RetransmissionAcknowledge(IHS_SessionRetransmission *retransmission,
                                   IHS_SessionChannelId channelId, uint16_t packetId,
                                   int16_t fragmentId, uint64_t nowMs) {
    IHS_MutexLock(retransmission->lock);
    IHS_RetransmissionPending **link = &retransmission->head;
    while (*link != NULL && !PacketIdentityMatches(&(*link)->packet.header, channelId,
                                                   packetId, fragmentId)) {
        link = &(*link)->next;
    }
    IHS_RetransmissionPending *pending = *link;
    if (pending != NULL) {
        *link = pending->next;
        retransmission->stats.acknowledged++;
        retransmission->stats.outstanding--;
        uint64_t latency = nowMs >= pending->firstTrackedMs ? nowMs - pending->firstTrackedMs : 0;
        if (latency > retransmission->stats.maxAckLatencyMs) {
            retransmission->stats.maxAckLatencyMs = latency;
        }
    }
    IHS_MutexUnlock(retransmission->lock);
    if (pending != NULL) {
        PendingDestroy(pending);
        return true;
    }
    return false;
}

bool IHS_RetransmissionSupersede(IHS_SessionRetransmission *retransmission,
                                 IHS_SessionChannelId channelId, uint16_t packetId,
                                 int16_t fragmentId) {
    bool found = false;
    IHS_MutexLock(retransmission->lock);
    for (IHS_RetransmissionPending *pending = retransmission->head; pending != NULL;
         pending = pending->next) {
        if (PacketIdentityMatches(&pending->packet.header, channelId, packetId, fragmentId)) {
            pending->superseded = true;
            found = true;
            break;
        }
    }
    IHS_MutexUnlock(retransmission->lock);
    return found;
}

bool IHS_RetransmissionNack(IHS_SessionRetransmission *retransmission,
                            IHS_SessionChannelId channelId, uint16_t packetId,
                            int16_t fragmentId, uint64_t nowMs) {
    bool found = false;
    IHS_MutexLock(retransmission->lock);
    for (IHS_RetransmissionPending *pending = retransmission->head; pending != NULL;
         pending = pending->next) {
        if (PacketIdentityMatches(&pending->packet.header, channelId, packetId, fragmentId)) {
            if (pending->initialSent) {
                pending->nextRetryMs = nowMs;
            } else {
                pending->nackPending = true;
            }
            retransmission->stats.nacks++;
            found = true;
            break;
        }
    }
    IHS_MutexUnlock(retransmission->lock);
    return found;
}

void IHS_RetransmissionNoteInitialSend(IHS_SessionRetransmission *retransmission,
                                       const IHS_SessionPacketHeader *header, bool sent,
                                       uint64_t nowMs) {
    IHS_MutexLock(retransmission->lock);
    for (IHS_RetransmissionPending *pending = retransmission->head; pending != NULL;
         pending = pending->next) {
        if (PacketIdentityMatches(&pending->packet.header, header->channelId,
                                  header->packetId, header->fragmentId)) {
            pending->initialSent = true;
            pending->lastSendMs = nowMs;
            if (!sent || pending->nackPending) {
                pending->nextRetryMs = nowMs;
            } else {
                pending->nextRetryMs = nowMs + RETRANSMISSION_INITIAL_MS;
            }
            if (!sent) {
                retransmission->stats.sendFailures++;
            }
            break;
        }
    }
    IHS_MutexUnlock(retransmission->lock);
}

bool IHS_RetransmissionIsTracked(const IHS_SessionRetransmission *retransmission,
                                 IHS_SessionChannelId channelId, uint16_t packetId,
                                 int16_t fragmentId) {
    IHS_MutexLock(retransmission->lock);
    for (const IHS_RetransmissionPending *pending = retransmission->head; pending != NULL;
         pending = pending->next) {
        if (PacketIdentityMatches(&pending->packet.header, channelId, packetId, fragmentId)) {
            IHS_MutexUnlock(retransmission->lock);
            return true;
        }
    }
    IHS_MutexUnlock(retransmission->lock);
    return false;
}

size_t IHS_RetransmissionProcessAt(IHS_SessionRetransmission *retransmission, uint64_t nowMs,
                                   IHS_RetransmissionSendFunction send, void *context) {
    size_t dueCount = 0;
    IHS_RetransmissionPending *retired = NULL;
    IHS_MutexLock(retransmission->lock);

    IHS_RetransmissionPending **link = &retransmission->head;
    while (*link != NULL) {
        IHS_RetransmissionPending *pending = *link;
        /* Only superseded packets are retired; unacked packets retransmit
         * until ACKed or NACK-released, matching the official client. */
        if (pending->superseded &&
            pending->retryCount >= RETRANSMISSION_SUPERSEDE_MIN_RETRIES) {
            *link = pending->next;
            pending->next = retired;
            retired = pending;
            retransmission->stats.superseded++;
            retransmission->stats.outstanding--;
            continue;
        }
        link = &pending->next;
    }

    for (IHS_RetransmissionPending *pending = retransmission->head; pending != NULL;
         pending = pending->next) {
        if (pending->initialSent && pending->nextRetryMs <= nowMs) {
            dueCount++;
        }
    }
    if (dueCount == 0) {
        IHS_MutexUnlock(retransmission->lock);
        while (retired != NULL) {
            IHS_RetransmissionPending *next = retired->next;
            PendingDestroy(retired);
            retired = next;
        }
        return 0;
    }

    IHS_SessionPacket *due = calloc(dueCount, sizeof(*due));
    if (due == NULL) {
        IHS_MutexUnlock(retransmission->lock);
        while (retired != NULL) {
            IHS_RetransmissionPending *next = retired->next;
            PendingDestroy(retired);
            retired = next;
        }
        return 0;
    }
    size_t index = 0;
    for (IHS_RetransmissionPending *pending = retransmission->head; pending != NULL;
         pending = pending->next) {
        if (!pending->initialSent || pending->nextRetryMs > nowMs) {
            continue;
        }
        PacketClone(&due[index], &pending->packet);
        pending->retryCount++;
        due[index].header.retransmitCount = pending->retryCount > UINT8_MAX
                                           ? UINT8_MAX : (uint8_t) pending->retryCount;
        pending->lastSendMs = nowMs;
        pending->nextRetryMs = nowMs + RetryDelayMs(pending->retryCount);
        retransmission->stats.retries++;
        index++;
    }
    IHS_MutexUnlock(retransmission->lock);

    while (retired != NULL) {
        IHS_RetransmissionPending *next = retired->next;
        PendingDestroy(retired);
        retired = next;
    }

    for (index = 0; index < dueCount; index++) {
        bool sent = send(&due[index], context);
        if (!sent) {
            IHS_MutexLock(retransmission->lock);
            retransmission->stats.sendFailures++;
            IHS_MutexUnlock(retransmission->lock);
        }
        IHS_SessionPacketClear(&due[index], true);
    }
    free(due);
    return dueCount;
}

void IHS_RetransmissionGetStats(IHS_SessionRetransmission *retransmission,
                                IHS_RetransmissionStats *stats, uint64_t nowMs) {
    IHS_MutexLock(retransmission->lock);
    *stats = retransmission->stats;
    uint64_t oldest = nowMs;
    bool found = false;
    for (IHS_RetransmissionPending *pending = retransmission->head; pending != NULL;
         pending = pending->next) {
        if (!found || pending->firstTrackedMs < oldest) {
            oldest = pending->firstTrackedMs;
            found = true;
            stats->oldestChannelId = pending->packet.header.channelId;
            stats->oldestPacketId = pending->packet.header.packetId;
            stats->oldestFragmentId = pending->packet.header.fragmentId;
            stats->oldestRetryCount = pending->retryCount;
        }
    }
    stats->oldestOutstandingMs = found && nowMs >= oldest ? nowMs - oldest : 0;
    IHS_MutexUnlock(retransmission->lock);
}

size_t IHS_RetransmissionAcknowledgeThrough(IHS_SessionRetransmission *retransmission,
                                            IHS_SessionChannelId channelId, uint16_t packetId,
                                            uint64_t nowMs) {
    size_t released = 0;
    IHS_MutexLock(retransmission->lock);
    IHS_RetransmissionPending **link = &retransmission->head;
    while (*link != NULL) {
        IHS_RetransmissionPending *pending = *link;
        if (pending->packet.header.channelId != channelId) {
            link = &pending->next;
            continue;
        }
        uint16_t below = (uint16_t) (packetId - pending->packet.header.packetId);
        if (below == 0 || below >= 0x8000u) {
            /* At/after the boundary (or equal): not strictly below. */
            link = &pending->next;
            continue;
        }
        *link = pending->next;
        retransmission->stats.acknowledged++;
        retransmission->stats.outstanding--;
        PendingDestroy(pending);
        released++;
        continue;
    }
    IHS_MutexUnlock(retransmission->lock);
    (void) nowMs;
    return released;
}

size_t IHS_RetransmissionNackAllThrough(IHS_SessionRetransmission *retransmission,
                                        IHS_SessionChannelId channelId, uint16_t packetId,
                                        uint64_t nowMs) {
    size_t nudged = 0;
    IHS_MutexLock(retransmission->lock);
    for (IHS_RetransmissionPending *pending = retransmission->head; pending != NULL;
         pending = pending->next) {
        if (pending->packet.header.channelId != channelId) {
            continue;
        }
        uint16_t below = (uint16_t) (packetId - pending->packet.header.packetId);
        if (below >= 0x8000u) {
            /* Above the reference: not covered by this NACK. */
            continue;
        }
        if (pending->initialSent) {
            pending->nextRetryMs = nowMs;
        } else {
            pending->nackPending = true;
        }
        retransmission->stats.nacks++;
        nudged++;
    }
    IHS_MutexUnlock(retransmission->lock);
    return nudged;
}
