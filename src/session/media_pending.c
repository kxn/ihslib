/* Official client HandlePendingDataPackets 7aff74: snapshot all complete
 * packets, empty the live list, then re-enter OnStreamPacket for every item.
 * Unmatched complete packets may requeue; Pause or phase0/7 discards them.
 * Raw early 3/4/11 storage is still a separate, explicitly documented gap. */
#include "channels/channel.h"
#include "session_pri.h"
#include "session_queue.h"
#include <string.h>

void IHS_SessionDrainPendingData(IHS_Session *session, IHS_SessionChannel *channel) {
    QueuedPacket *snapshot = NULL, **tail = &snapshot, *queued;
    /* Detach before invoking any delegate. A packet requeued by a delegate is
     * a new live-list item and must never be replayed again in this snapshot. */
    while ((queued = (void *)IHS_QueuePoll(session->pendingData))) {
        session->pendingDataCount--;
        queued->batchNext = NULL;
        *tail = queued;
        tail = &queued->batchNext;
    }
    while ((queued = snapshot)) {
        snapshot = queued->batchNext;
        queued->batchNext = NULL;
        if (queued->pendingReliableFrame) {
            IHS_SessionFrame frame = {.header = queued->packet.header, .body = queued->packet.body};
            memset(&queued->packet.body, 0, sizeof(queued->packet.body));
            IHS_SessionDeliverReliableFrame(session, &frame);
            IHS_SessionFrameClear(&frame, true);
        } else if (queued->packet.header.channelId == channel->id) {
            IHS_SessionChannelReceivedPacket(channel, &queued->packet);
        } else {
            /* This legacy raw-packet cache is not an official complete-frame
             * pending item. Do not inject it into a reliable frame dispatcher. */
            IHS_QueueAppend(session->pendingData, (void *)queued);
            session->pendingDataCount++;
            continue;
        }
        IHS_SessionPacketClear(&queued->packet, true);
        IHS_QueueItemFree((void *)queued);
    }
}
