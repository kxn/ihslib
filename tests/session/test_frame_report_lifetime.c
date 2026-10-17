/* Local resource-failure/lifetime check, not an official OOM policy oracle.
 * Real StatsSend triggers synchronous disconnect after rejected preparation. */
#include "ihslib.h"
#include "session/frame_stats.h"
#include "test_session.h"
#include <assert.h>
#include <stdlib.h>

static IHS_Session *active;
static unsigned attempts;
IHS_SendAdmission __wrap_IHS_SessionChannelQueueFrameResult(IHS_SessionChannel *channel,
                                                            IHS_SessionFrame *frame,
                                                            bool reliable) {
    assert(channel->id == 2 && reliable && frame->body.size > 1);
    ++attempts;
    IHS_FrameStatsAggregator *agg = active->frameStats;
    assert(agg->reportPending && agg->lastSentFrameId == 1);
    assert(agg->pendingReport.serial == 1); /* local token may wrap, reports continue */
    /* Completing the failed old borrow must preserve next-interval vectors. */
    IHS_MutexLock(agg->lock);
    assert(IHS_FrameStatsAccumulatorAdd(&agg->accumulator, 6, 42));
    IHS_MutexUnlock(agg->lock);
    return IHS_SendAdmissionLocalFailure;
}
int main(void) {
    IHS_Init();
    active = IHS_TestSessionCreate();
    assert(active);
    IHS_TimerTaskStopImmediate(active->retransmission.timer);
    active->state.connectionState = IHS_SessionConnectionStateConnected;
    active->state.hostConnectionId = 34;
    IHS_FrameStatsAggregator *agg = active->frameStats;
    agg->reportSerial = UINT64_MAX;
    IHS_FrameStatsRecordReceived(agg, 1, 100, 110, 120, 20, 0);
    IHS_FrameStatsRecordCompleteAt(agg, 1, IHS_VideoFrameResultDisplayed, 130);
    IHS_FrameStatsMessage *scratch = calloc(1, sizeof(*scratch));
    assert(scratch);
    assert(IHS_SessionSendFrameEvents(active, scratch));
    assert(attempts == 1 && active->transport.shutdown);
    assert(!agg->reportPending && agg->lastQueuedFrameId == 0);
    assert(agg->accumulator.slots[6].count == 1);
    assert(agg->accumulator.slots[6].samples[0] == 42);
    assert(!IHS_FrameStatsReportBegin(agg)); /* failed report is never replayed */
    free(scratch);
    IHS_SessionDestroy(active);
    IHS_Quit();
    return 0;
}
