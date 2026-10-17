/* Official client HandlePendingDataPackets 7aff74: snapshot all complete
 * packets, empty the live list, then re-enter OnStreamPacket for every item.
 * Unmatched complete packets may requeue; Pause or phase0/7 discards them.
 * All carriers have already completed transport reassembly at this boundary. */
#include "channels/channel.h"
#include "session_pri.h"
#include "session_queue.h"
#include <string.h>

void IHS_SessionDrainPendingData(IHS_Session *session, IHS_SessionChannel *channel) {
    (void)channel; /* A configuration replays every channel, as the original does. */
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
        IHS_SessionFrame frame = {.header = queued->packet.header, .body = queued->packet.body};
        memset(&queued->packet.body, 0, sizeof(queued->packet.body));
        IHS_SessionDeliverReliableFrame(session, &frame);
        IHS_SessionFrameClear(&frame, true);
        IHS_SessionPacketClear(&queued->packet, true);
        IHS_QueueItemFree((void *)queued);
    }
}
