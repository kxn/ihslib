/*
 *  _____  _   _  _____  _  _  _
 * |_   _|| | | |/  ___|| |(_)| |     Steam
 *   | |  | |_| |\ `--. | | _ | |__     In-Home
 *   | |  |  _  | `--. \| || || '_ \      Streaming
 *  _| |_ | | | |/\__/ /| || || |_) |       Library
 *  \___/ \_| |_/\____/ |_||_||_.__/
 *
 * Copyright (c) 2022 Mariotaku <https://github.com/mariotaku>.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "retransmission.h"
#include "session_pri.h"

#define RETRANSMISSION_INTERVAL 10
#define RETRANSMISSION_ATTEMPTS 20

typedef struct IHS_QueueItem {
    IHS_SessionPacket packet;
    IHS_SessionRetransmission *retransmission;
    bool cancelled;
} PendingRetransmission;

typedef struct RetransmissionQuery {
    IHS_SessionChannelId channelId;
    uint16_t packetId;
    uint16_t fragmentId;
} RetransmissionQuery;

static uint64_t RetransmissionTimerRun(int runCount, void *context);

static void RetransmissionTimerEnd(void *context);

static bool RetransmissionIdenticalPredicate(PendingRetransmission *item, void *context);

static bool RetransmissionPacketPredicate(PendingRetransmission *item, void *context);

void IHS_RetransmissionInit(IHS_SessionRetransmission *retransmission, IHS_Session *session) {
    retransmission->session = session;
    retransmission->lock = IHS_MutexCreate();
    retransmission->queue = IHS_QueueCreate(sizeof(PendingRetransmission));
}

void IHS_RetransmissionDeinit(IHS_SessionRetransmission *retransmission) {
    /* The session destroys its timers first, so every task has already run its end
     * callback and drained itself out of this queue. Nothing left to destroy. */
    IHS_MutexLock(retransmission->lock);
    IHS_QueueDestroy(retransmission->queue, NULL, NULL);
    IHS_MutexUnlock(retransmission->lock);
    IHS_MutexDestroy(retransmission->lock);
}

bool IHS_RetransmissionQueue(IHS_SessionRetransmission *retransmission, IHS_SessionPacket *packet) {
    assert(packet->body.data != NULL);
    assert(packet->body.offset == IHS_PACKET_HEADER_SIZE);
    if (packet->header.retransmitCount >= RETRANSMISSION_ATTEMPTS) {
        return false; /* RetransmissionTimerRun already warned */
    }
    PendingRetransmission *pending = IHS_QueueItemObtain(retransmission->queue);
    pending->packet = *packet;
    pending->retransmission = retransmission;
    pending->packet.header.retransmitCount++;
    IHS_BufferTransferOwnership(&packet->body, &pending->packet.body);
    IHS_TimerTaskStart(retransmission->session->timers, RetransmissionTimerRun, RetransmissionTimerEnd,
                       RETRANSMISSION_INTERVAL, pending);
    IHS_MutexLock(retransmission->lock);
    IHS_QueueAppend(retransmission->queue, pending);
    IHS_MutexUnlock(retransmission->lock);
    IHS_SessionLog(retransmission->session, IHS_LogLevelVerbose, "Retransmission",
                   "Queued Packet(channelId=%u, packetId=%u, fragmentId=%u), retransmitCount=%u",
                   pending->packet.header.channelId, pending->packet.header.packetId,
                   pending->packet.header.fragmentId, pending->packet.header.retransmitCount);
    return true;
}

bool IHS_RetransmissionCancel(IHS_SessionRetransmission *retransmission, IHS_SessionChannelId channelId,
                              uint16_t packetId, uint16_t fragmentId) {
    RetransmissionQuery query = {
            .channelId = channelId,
            .packetId = packetId,
            .fragmentId = fragmentId,
    };
    /* Cancel every match, not just the first: a packet awaiting retransmission has two
     * entries for a moment, since RetransmissionTimerRun re-queues it with
     * retransmitCount+1 before RetransmissionTimerEnd removes the old entry. An ACK
     * landing in that window used to leave one twin retransmitting to the limit.
     *
     * The timer thread owns every pending item: it alone frees them, from the task's
     * end callback. So cancelling must not stop the task nor touch the item once the
     * lock is dropped — an ACK arriving as the task expires would otherwise read
     * match->task out of a pending that RetransmissionTimerEnd has already freed, and
     * stop an IHS_TimerTask that died with it. Marking under the lock is enough: the
     * next run sees the flag, sends nothing, and ends, freeing the item on the thread
     * that owns it. Cost is one RETRANSMISSION_INTERVAL of lingering, not a segfault. */
    bool cancelled = false;
    for (;;) {
        IHS_MutexLock(retransmission->lock);
        PendingRetransmission *match = IHS_QueuePollBy(retransmission->queue, RetransmissionPacketPredicate, &query);
        unsigned int retransmitCount = 0;
        if (match != NULL) {
            match->cancelled = true;
            retransmitCount = match->packet.header.retransmitCount;
        }
        IHS_MutexUnlock(retransmission->lock);
        if (match == NULL) {
            break;
        }
        cancelled = true;
        IHS_SessionLog(retransmission->session, IHS_LogLevelVerbose, "Retransmission",
                       "Cancelling Packet(channelId=%u, packetId=%u, fragmentId=%u), retransmitCount=%u",
                       channelId, packetId, fragmentId, retransmitCount);
    }
    return cancelled;
}

static uint64_t RetransmissionTimerRun(int runCount, void *context) {
    (void) runCount;
    PendingRetransmission *pending = context;
    IHS_SessionRetransmission *retransmission = pending->retransmission;
    IHS_MutexLock(retransmission->lock);
    bool cancelled = pending->cancelled;
    IHS_MutexUnlock(retransmission->lock);
    if (cancelled) {
        return 0; /* ACKed: end the task, which frees the packet in the end callback */
    }
    IHS_SessionPacket *packet = &pending->packet;
    bool again = packet->header.retransmitCount < RETRANSMISSION_ATTEMPTS;
    if (!again) {
        /* Twenty unacknowledged copies of one packet is not a lossy link, it is a
         * packet the peer never accepts. Say so, instead of falling silent. */
        IHS_SessionLog(retransmission->session, IHS_LogLevelWarn, "Retransmission",
                       "Giving up on Packet(channelId=%u, packetId=%u, fragmentId=%u) after %u attempts",
                       packet->header.channelId, packet->header.packetId, packet->header.fragmentId,
                       RETRANSMISSION_ATTEMPTS);
    }
    IHS_SessionQueuePacket(retransmission->session, packet, again);
    assert(packet->body.data == NULL);
    return 0;
}

static void RetransmissionTimerEnd(void *context) {
    PendingRetransmission *pending = context;
    IHS_SessionRetransmission *retransmission = pending->retransmission;
    IHS_SessionLog(retransmission->session, IHS_LogLevelVerbose, "Retransmission",
                   "Timer ended for Packet(channelId=%u, packetId=%u, fragmentId=%u), retransmitCount=%u",
                   pending->packet.header.channelId, pending->packet.header.packetId, pending->packet.header.fragmentId,
                   pending->packet.header.retransmitCount);
    IHS_MutexLock(retransmission->lock);
    IHS_QueuePollBy(retransmission->queue, RetransmissionIdenticalPredicate, pending);
    IHS_MutexUnlock(retransmission->lock);
    IHS_SessionPacketClear(&pending->packet, true);
    IHS_QueueItemFree(pending);
}

static bool RetransmissionIdenticalPredicate(PendingRetransmission *item, void *context) {
    return item == context;
}

static bool RetransmissionPacketPredicate(PendingRetransmission *item, void *context) {
    RetransmissionQuery *query = context;
    return item->packet.header.channelId == query->channelId &&
           item->packet.header.packetId == query->packetId &&
           item->packet.header.fragmentId == query->fragmentId;
}