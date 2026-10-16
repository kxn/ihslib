/*
 * Copyright (c) 2022 Mariotaku <https://github.com/mariotaku>.
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "retransmission.h"

#include <assert.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>

#include "session_pri.h"

#define RETRANSMISSION_TICK_MS 5
#define RETRANSMISSION_INITIAL_MS 25
/* Official pacing: timeout = 1.25 x clamp(conn timeout estimate, 65..3276
 * (0.99..50.0 ms in 16.16s units)) -- libmain 0x7f8ac4 with the static
 * initializer at 0x7fe284 ([f47000+2436]=65, [+2440]=3276).
 * RetryDelayMs obtains the rolling estimate from ACK clock feedback. */
/* No give-up: the official client retransmits reliable packets until they are
 * ACKed or released by a peer NACK (libmain.so 0x7f8ac4 SendReliablePackets /
 * 0x7f94b8 HandleAckPacket / 0x7f95dc HandleNackPacket contain no retire
 * path). The host delivers encrypted frames strictly in order, so a delayed
 * retransmit keeps a valid sequence number and heals the hole. The former
 * 3 s give-up created permanent holes. Whether this explains the observed
 * ~20 s input stall requires device evidence. See protocol RE §14. */

struct IHS_RetransmissionPending {
    IHS_SessionPacket packet;
    uint64_t firstTrackedMs;
    uint64_t lastSendMs;
    uint32_t lastSendTimestamp;
    uint64_t nextRetryMs;
    uint32_t retryCount;
    bool initialSent;
    bool nackPending;
    bool tracked;
    atomic_uint references;
    uint8_t nextWireCount;
    uint64_t feedbackGeneration;
    IHS_RetransmissionPending *next;
};

static bool PacketIdentityMatches(const IHS_SessionPacketHeader *header,
                                  IHS_SessionChannelId channelId, uint16_t packetId,
                                  int16_t fragmentId) {
    return header->channelId == channelId && header->packetId == packetId &&
           (fragmentId == INT16_MIN || header->fragmentId == fragmentId);
}

static void PacketClone(IHS_SessionPacket *dest, const IHS_SessionPacket *source) {
    memset(dest, 0, sizeof(*dest));
    dest->header = source->header;
    dest->crc = source->crc;
    IHS_SessionPacketBodyInitialize(&dest->body, dest->header.hasCrc);
    IHS_BufferAppendMem(&dest->body, IHS_BufferPointer(&source->body), source->body.size);
}

static void PendingDestroy(IHS_RetransmissionPending *pending) {
    if (atomic_fetch_sub(&pending->references, 1) != 1) return;
    IHS_SessionPacketClear(&pending->packet, true);
    free(pending);
}

static uint64_t RetryDelayMs(IHS_SessionRetransmission *retransmission) {
    if (!retransmission->session) return RETRANSMISSION_INITIAL_MS;
    uint32_t ticks = IHS_StreamClockRetryTicks(&retransmission->session->clock);
    return ((uint64_t) ticks * 1000 + 65535) / 65536;
}

static bool SendRetry(IHS_SessionPacket *packet, void *context) {
    return IHS_SessionSendPacket(context, packet);
}

static uint64_t RetransmissionTimerRun(int runCount, void *context) {
    (void) runCount;
    IHS_SessionRetransmission *retransmission = context;
    IHS_RetransmissionProcessTicks(retransmission, IHS_TimerNow(),
                                   IHS_SessionPacketTimestamp(), SendRetry, retransmission->session);
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
    retransmission->sendLock = IHS_MutexCreate();
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
    IHS_MutexDestroy(retransmission->sendLock);
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
    atomic_init(&pending->references, 1);
    PacketClone(&pending->packet, packet);
    pending->nextWireCount = packet->header.retransmitCount;
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
    pending->tracked = true;
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
        pending->tracked = false;
        retransmission->stats.acknowledged++;
        if (pending->packet.header.hidReport) retransmission->stats.hidAcknowledged++;
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
            /* Compatibility API: a newer report cannot cancel a reliable packet. */
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
                pending->nackPending = true;
            } else {
                pending->nackPending = true;
            }
            pending->feedbackGeneration++;
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
            pending->lastSendTimestamp = header->sendTimestamp;
            pending->packet.header.sendTimestamp = header->sendTimestamp;
            pending->nextWireCount = header->retransmitCount;
            if (sent && pending->nextWireCount < UINT8_MAX) pending->nextWireCount++;
            if (!sent || pending->nackPending) {
                pending->nextRetryMs = nowMs;
            } else {
                pending->nextRetryMs = nowMs + RetryDelayMs(retransmission);
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

static bool PendingDue(IHS_RetransmissionPending *p, uint64_t nowMs, bool ticks,
                        uint32_t nowTicks, uint32_t delay) {
    if (!p->initialSent) return false;
    /* Connect has a different official scheduler; preserve its existing path
     * until connection-level connect retries are moved out of this window. */
    if (!ticks || (p->packet.header.type != IHS_SessionPacketTypeReliable &&
                   p->packet.header.type != IHS_SessionPacketTypeReliableFrag))
        return p->nextRetryMs <= nowMs;
    return p->nackPending || !p->nextWireCount || !p->lastSendTimestamp ||
           (int32_t)(p->lastSendTimestamp + delay - nowTicks) < 0;
}
static size_t Process(IHS_SessionRetransmission *retransmission, uint64_t nowMs,
                       bool ticks, uint32_t nowTicks,
                       IHS_RetransmissionSendFunction send, void *context) {
    typedef struct {
        IHS_RetransmissionPending *pending;
        IHS_SessionPacket packet;
        uint64_t feedbackGeneration;
    } Attempt;
    IHS_MutexLock(retransmission->sendLock);
    /* Snapshot the estimate once: clock feedback has its own lock, so using a
     * changing estimate between count and fill could exceed the allocation. */
    uint32_t delay = retransmission->session ?
        IHS_StreamClockRetryTicks(&retransmission->session->clock) : 1638;
    IHS_MutexLock(retransmission->lock);
    size_t dueCount = 0;
    for (IHS_RetransmissionPending *p = retransmission->head; p; p = p->next)
        if (PendingDue(p, nowMs, ticks, nowTicks, delay)) dueCount++;
    Attempt *due = dueCount ? calloc(dueCount, sizeof(*due)) : NULL;
    if (!due) {
        IHS_MutexUnlock(retransmission->lock);
        IHS_MutexUnlock(retransmission->sendLock);
        return 0;
    }
    size_t index = 0;
    for (IHS_RetransmissionPending *p = retransmission->head; p; p = p->next) {
        if (!PendingDue(p, nowMs, ticks, nowTicks, delay)) continue;
        Attempt *a = &due[index++];
        a->pending = p;
        atomic_fetch_add(&p->references, 1);
        a->feedbackGeneration = p->feedbackGeneration;
        PacketClone(&a->packet, &p->packet);
        a->packet.header.retransmitCount = p->nextWireCount;
    }
    IHS_MutexUnlock(retransmission->lock);

    size_t attempted = 0;
    bool failed[256] = {false};
    /* The official sender walks oldest-first and stops this channel on the
     * first nonzero transport result (0x7f8b94..d4). Other channels still run.
     * Snapshot references allow ACK to unlink/free its ownership during I/O;
     * skipped/acknowledged entries never have counters or deadlines advanced. */
    for (size_t remaining = dueCount; remaining > 0;) {
        Attempt *a = &due[--remaining];
        IHS_RetransmissionPending *p = a->pending;
        unsigned channel = a->packet.header.channelId;
        IHS_MutexLock(retransmission->lock);
        bool eligible = p->tracked && !failed[channel];
        IHS_MutexUnlock(retransmission->lock);
        if (eligible) {
            bool sent = send(&a->packet, context);
            attempted++;
            failed[channel] = !sent;
            IHS_MutexLock(retransmission->lock);
            retransmission->stats.retries++;
            if (!sent) retransmission->stats.sendFailures++;
            if (p->tracked) {
                p->lastSendMs = nowMs;
                p->lastSendTimestamp = a->packet.header.sendTimestamp;
                p->packet.header.sendTimestamp = a->packet.header.sendTimestamp;
                /* Socket stamps even a rejected/failed attempt. Only success
                 * increments the stored wire count; it saturates at 255. */
                if (sent) {
                    p->retryCount = a->packet.header.retransmitCount;
                    if (p->nextWireCount < UINT8_MAX) p->nextWireCount++;
                }
                if (p->feedbackGeneration == a->feedbackGeneration) {
                    p->nackPending = false;
                    p->nextRetryMs = !p->nextWireCount ? nowMs :
                        nowMs + RetryDelayMs(retransmission);
                }
            }
            IHS_MutexUnlock(retransmission->lock);
        }
        IHS_SessionPacketClear(&a->packet, true);
        PendingDestroy(p);
    }
    free(due);
    IHS_MutexUnlock(retransmission->sendLock);
    return attempted;
}

size_t IHS_RetransmissionProcessAt(IHS_SessionRetransmission *r, uint64_t nowMs,
                                   IHS_RetransmissionSendFunction send, void *context) {
    return Process(r, nowMs, false, 0, send, context);
}
size_t IHS_RetransmissionProcessTicks(IHS_SessionRetransmission *r, uint64_t nowMs,
                                      uint32_t nowTicks, IHS_RetransmissionSendFunction send,
                                      void *context) {
    return Process(r, nowMs, true, nowTicks, send, context);
}

void IHS_RetransmissionGetStats(IHS_SessionRetransmission *retransmission,
                                IHS_RetransmissionStats *stats, uint64_t nowMs) {
    IHS_MutexLock(retransmission->lock);
    *stats = retransmission->stats;
    stats->hidPending = stats->hidInFlight = 0;
    stats->hidOldestInFlightPacketId = -1;
    uint64_t oldestHID = UINT64_MAX;
    uint64_t oldest = nowMs;
    bool found = false;
    for (IHS_RetransmissionPending *pending = retransmission->head; pending != NULL;
         pending = pending->next) {
        if (pending->packet.header.hidReport) {
            if (pending->initialSent) {
                stats->hidInFlight++;
                if (pending->firstTrackedMs <= oldestHID) {
                    oldestHID = pending->firstTrackedMs;
                    stats->hidOldestInFlightPacketId = pending->packet.header.packetId;
                }
            } else stats->hidPending++;
        }
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
        pending->tracked = false;
        retransmission->stats.acknowledged++;
        if (pending->packet.header.hidReport) retransmission->stats.hidAcknowledged++;
        retransmission->stats.outstanding--;
        uint64_t latency = nowMs >= pending->firstTrackedMs ? nowMs - pending->firstTrackedMs : 0;
        if (latency > retransmission->stats.maxAckLatencyMs) {
            retransmission->stats.maxAckLatencyMs = latency;
        }
        PendingDestroy(pending);
        released++;
        continue;
    }
    IHS_MutexUnlock(retransmission->lock);
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
        if (below == 0 || below >= 0x8000u) {
            /* Above the reference: not covered by this NACK. */
            continue;
        }
        if (pending->initialSent) {
            pending->nextRetryMs = nowMs;
            pending->nackPending = true;
        } else {
            pending->nackPending = true;
        }
        pending->feedbackGeneration++;
        retransmission->stats.nacks++;
        nudged++;
    }
    IHS_MutexUnlock(retransmission->lock);
    return nudged;
}

size_t IHS_RetransmissionNackBefore(IHS_SessionRetransmission *r, IHS_SessionChannelId channel,
                                    uint16_t id, bool below, uint32_t cutoff, uint64_t nowMs) {
    size_t count = 0;
    IHS_MutexLock(r->lock);
    for (IHS_RetransmissionPending *p = r->head; p; p = p->next) {
        uint16_t distance = (uint16_t)(id - p->packet.header.packetId);
        if (p->packet.header.channelId != channel ||
            (below ? (distance == 0 || distance >= 0x8000u) : distance != 0)) continue;
        /* Never retransmit a still-queued original or a packet newer than
         * the combined age/peer-observation cutoff (0x7f976c..80). */
        if (!p->initialSent || (int32_t)(p->lastSendTimestamp - cutoff) > 0) continue;
        p->nextRetryMs = nowMs;
        p->nackPending = true;
        p->feedbackGeneration++;
        r->stats.nacks++; count++;
    }
    IHS_MutexUnlock(r->lock);
    return count;
}
