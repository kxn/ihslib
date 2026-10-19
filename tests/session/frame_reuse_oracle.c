/* Local ownership invariant, not an official protocol/window constant.
 * Use submitted/completed inputs, release references, then run the actual
 * collector. Unsubmitted pending retirement cannot satisfy this workload. */
#include "ihslib.h"
#include "session/frame_stats.h"
#include "session/frame_tracker.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    IHS_Init();
    IHS_FrameTracker *tracker = IHS_FrameTrackerCreate(93);
    IHS_FrameStatsAggregator *stats = IHS_FrameStatsAggregatorCreate();
    assert(tracker && stats && IHS_FrameTrackerOpenEpoch(tracker, 1));
    for (unsigned n = 0; n < 300; ++n) {
        IHS_FrameReceive receive = {.firstReceiveUs = n, .lastReceiveUs = n};
        IHS_FrameTicket *ticket;
        assert(IHS_FrameTrackerBeginInput(tracker, 1, n, &receive, &ticket) == IHS_FrameBeginOK);
        assert(IHS_FrameTicketVideoDecodeStage(ticket, false));
        assert(IHS_FrameTicketVideoDecodeStage(ticket, true));
        IHS_FrameOutcome outcome = {.result = IHS_VideoFrameResultDroppedReset};
        assert(IHS_FrameTicketComplete(ticket, &outcome));
        IHS_FrameTicketRelease(ticket);
        IHS_FrameStatsCollectTracked(stats, tracker);
    }
    printf("{\"submitted\":300,\"storage\":%zu}\n", IHS_FrameTrackerStorageCapacity(tracker));
    IHS_FrameStatsAggregatorDestroy(stats);
    IHS_FrameTrackerClose(tracker);
    IHS_Quit();
    return 0;
}
