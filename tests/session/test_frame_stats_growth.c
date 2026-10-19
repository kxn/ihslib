/* Storage pressure must not omit completions/stages in an overflow block or
 * consume them on failed scratch growth. External tickets remain stable. */
#include "ihslib.h"
#include "session/frame_stats.h"
#include "session/frame_tracker.h"
#include <assert.h>
#include <stdlib.h>

static bool failScratch;
void *__real_realloc(void *, size_t);
void *__wrap_realloc(void *p, size_t size) {
    if (failScratch) {
        failScratch = false;
        return NULL;
    }
    return __real_realloc(p, size);
}
int main(void) {
    IHS_Init();
    IHS_FrameTracker *tracker = IHS_FrameTrackerCreate(9);
    IHS_FrameStatsAggregator *agg = IHS_FrameStatsAggregatorCreate();
    assert(tracker && agg && IHS_FrameTrackerOpenEpoch(tracker, 1));
    IHS_FrameStatsCollectTracked(agg, tracker);
    IHS_FrameTicket *tickets[300];
    for (unsigned i = 0; i < 300; ++i) {
        IHS_FrameReceive received = {.firstReceiveUs = 1, .lastReceiveUs = 1};
        IHS_FrameStatsRecordReceived(agg, i + 1, 100 + i, 200 + i, 300 + i, 10, 0);
        assert(IHS_FrameTrackerBegin(tracker, 1, i + 1, &received, &tickets[i]) ==
               IHS_FrameBeginOK);
    }
    assert(IHS_FrameTrackerStorageCapacity(tracker) == 512);
    assert(IHS_FrameTicketDecodeStage(tickets[299], false, 1000));
    assert(IHS_FrameTicketDecodeStage(tickets[299], true, 2000));
    IHS_FrameOutcome shown = {
        .result = IHS_VideoFrameResultDisplayed, .presentationSerial = 1, .completionUs = 3000};
    assert(IHS_FrameTicketComplete(tickets[299], &shown));
    shown.presentationSerial = 2;
    assert(IHS_FrameTicketComplete(tickets[249], &shown));
    failScratch = true;
    IHS_FrameStatsCollectTracked(agg, tracker);
    assert(!failScratch && agg->trackedCapacity == 256 && agg->lastDisplayedFrameId == 0);
    assert(!agg->ring[300 % IHS_FRAME_STATS_RING_SIZE].complete);
    IHS_FrameStatsCollectTracked(agg, tracker);
    /* Physical block traversal visits 250 before 300. Completion order is the
     * reverse, so latest Displayed must be 250 while 300 is also collected. */
    assert(agg->trackedCapacity == 512 && agg->lastDisplayedFrameId == 250);
    IHS_FrameStatsSlot *slot = &agg->ring[300 % IHS_FRAME_STATS_RING_SIZE];
    assert(slot->frameId == 300 && slot->complete && slot->result == IHS_VideoFrameResultDisplayed);
    assert(slot->events[14] == 65 && slot->events[15] == 131);
    for (unsigned i = 0; i < 299; ++i)
        IHS_FrameTicketRelease(tickets[i]);
    IHS_FrameTrackerClose(tracker);
    IHS_FrameStatsAggregatorDestroy(agg);
    IHS_Quit();
    /* A block ticket survives its session/owner shutdown until final release. */
    assert(IHS_FrameTicketIdentity(tickets[299]).frameId == 300);
    IHS_FrameTicketRelease(tickets[299]);
    return 0;
}
