#include "ihslib.h"
#include "session/frame_stats.h"
#include "session/frame_tracker.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdlib.h>

static IHS_FrameTicket *receive(IHS_FrameStatsAggregator *agg, IHS_FrameTracker *t, uint16_t id) {
    IHS_FrameStatsCollectTracked(agg, t);
    IHS_FrameStatsRecordReceived(agg, id, 100 + id, 200 + id, 300 + id, 100, 0);
    IHS_FrameReceive info = {.firstReceiveUs = 1, .lastReceiveUs = 1};
    IHS_FrameTicket *r = NULL;
    assert(IHS_FrameTrackerBegin(t, 1, id, &info, &r) == IHS_FrameBeginOK);
    return r;
}
static void bounded_no_display(void) {
    IHS_FrameStatsAggregator *agg = IHS_FrameStatsAggregatorCreate();
    IHS_FrameTracker *t = IHS_FrameTrackerCreate(1);
    assert(agg && t && IHS_FrameTrackerOpenEpoch(t, 1));
    for (unsigned i = 1; i <= 1000; ++i) {
        IHS_FrameTicket *r = receive(agg, t, i);
        IHS_FrameOutcome dropped = {.result = IHS_VideoFrameResultDroppedNetworkLost};
        assert(IHS_FrameTicketComplete(r, &dropped));
        IHS_FrameTicketRelease(r);
        if (i % 16 == 0)
            IHS_FrameStatsSettleTracked(agg, t, 100);
        assert(!IHS_FrameStatsReportBegin(agg));
        for (unsigned stat = 0; stat < IHS_FRAME_STATS_ACCUM_SLOTS; ++stat)
            assert(agg->accumulator.slots[stat].count == 0); /* no unbounded sample queue */
    }
    IHS_FrameTicket *r = receive(agg, t, 1001);
    IHS_FrameOutcome shown = {.result = IHS_VideoFrameResultDisplayed, .presentationSerial = 1};
    assert(IHS_FrameTicketComplete(r, &shown));
    IHS_FrameStatsCollectTracked(agg, t);
    const IHS_FrameStatsReport *report = IHS_FrameStatsReportBegin(agg);
    assert(report && report->count == 240 && report->accumulator.slots[6].count == 240);
    assert(IHS_FrameStatsReportFinish(agg, report->serial, true));
    IHS_FrameTicketRelease(r);
    IHS_FrameTrackerClose(t);
    IHS_FrameStatsAggregatorDestroy(agg);
}
static void cancellation_is_not_resource_expiry(void) {
    IHS_FrameStatsAggregator *agg = IHS_FrameStatsAggregatorCreate();
    IHS_FrameTracker *t = IHS_FrameTrackerCreate(2);
    assert(agg && t && IHS_FrameTrackerOpenEpoch(t, 1));
    IHS_FrameTicket *a = receive(agg, t, 1), *b = receive(agg, t, 2);
    IHS_FrameOutcome shown = {.result = IHS_VideoFrameResultDisplayed, .presentationSerial = 1};
    assert(IHS_FrameTicketComplete(b, &shown));
    IHS_FrameStatsCollectTracked(agg, t);
    const IHS_FrameStatsReport *report = IHS_FrameStatsReportBegin(agg);
    assert(report && report->count == 2 &&
           report->frames[0].result == IHS_VideoFrameResultDroppedLate);
    assert(IHS_FrameStatsReportFinish(agg, report->serial, true));
    /* Source Cancelled/reset was only a protocol event. The GPU holder can
     * still claim/publish its live ticket; it cannot revive retired stats. */
    assert(IHS_FrameTicketClaim(a));
    IHS_FrameTicketPublish(a, &shown);
    IHS_FrameStatsCollectTracked(agg, t);
    assert(agg->lastDisplayedFrameId == 2 && !IHS_FrameStatsReportBegin(agg));
    IHS_FrameTrackerClose(t);
    IHS_FrameStatsAggregatorDestroy(agg);
    IHS_FrameTicketRelease(a);
    IHS_FrameTicketRelease(b);
}
static void publish_between_collection_and_cleanup(void) {
    IHS_FrameTracker *t = IHS_FrameTrackerCreate(3);
    assert(t && IHS_FrameTrackerOpenEpoch(t, 1));
    IHS_FrameReceive info = {0};
    IHS_FrameTicket *r;
    assert(IHS_FrameTrackerBegin(t, 1, 1, &info, &r) == IHS_FrameBeginOK);
    IHS_TrackedFrame out;
    assert(IHS_FrameTrackerCopyStats(t, &out, 1) == 1 && !out.statsComplete);
    IHS_FrameOutcome shown = {.result = IHS_VideoFrameResultDisplayed, .presentationSerial = 1};
    assert(IHS_FrameTicketComplete(r, &shown));
    assert(IHS_FrameTrackerSettle(t, 100, 1000, &out, 1) == 0);
    assert(IHS_FrameTrackerCopyStats(t, &out, 1) == 1 && out.statsComplete);
    assert(IHS_FrameTrackerSettle(t, 100, 1000, &out, 1) == 1);
    IHS_FrameTrackerClose(t);
    IHS_FrameTicketRelease(r);
}

typedef struct concurrent_capture {
    IHS_FrameStatsAggregator *agg;
    IHS_FrameTracker *tracker;
    pthread_mutex_t lock;
    pthread_cond_t changed;
    IHS_FrameTicket *jobs[16];
    unsigned head, count;
    bool inputDone;
    atomic_bool published;
} concurrent_capture;
static void *publish_frames(void *context) {
    concurrent_capture *c = context;
    for (;;) {
        pthread_mutex_lock(&c->lock);
        while (!c->count && !c->inputDone)
            pthread_cond_wait(&c->changed, &c->lock);
        if (!c->count) {
            pthread_mutex_unlock(&c->lock);
            break;
        }
        IHS_FrameTicket *r = c->jobs[c->head++ % 16];
        --c->count;
        pthread_cond_broadcast(&c->changed);
        pthread_mutex_unlock(&c->lock);
        unsigned id = IHS_FrameTicketIdentity(r).frameId;
        assert(IHS_FrameTicketDecodeStage(r, false, id * 1000u));
        assert(IHS_FrameTicketDecodeStage(r, true, id * 1000u + 100));
        assert(IHS_FrameTicketUploadStage(r, false, id * 1000u + 200));
        assert(IHS_FrameTicketUploadStage(r, true, id * 1000u + 300));
        IHS_FrameOutcome outcome = {.result = IHS_VideoFrameResultDisplayed,
                                    .presentationSerial = id,
                                    .completionUs = id * 1000u + 500};
        assert(IHS_FrameTicketComplete(r, &outcome));
        IHS_FrameTicketRelease(r);
    }
    atomic_store(&c->published, true);
    return NULL;
}
static void emit_report(concurrent_capture *c, IHS_FrameStatsMessage *message) {
    IHS_FrameStatsCollectTracked(c->agg, c->tracker);
    const IHS_FrameStatsReport *report = IHS_FrameStatsReportBegin(c->agg);
    if (report) {
        assert(report->count <= 240);
        IHS_FrameStatsEncodeReport(report, 0, message);
        size_t size = cframe_stats_list_msg__get_packed_size(&message->message);
        uint8_t *bytes = malloc(size);
        assert(bytes && cframe_stats_list_msg__pack(&message->message, bytes) == size);
        CFrameStatsListMsg *decoded = cframe_stats_list_msg__unpack(NULL, size, bytes);
        assert(decoded && decoded->n_accumulated_stats >= 5);
        cframe_stats_list_msg__free_unpacked(decoded, NULL);
        free(bytes);
        assert(IHS_FrameStatsReportFinish(c->agg, report->serial, true));
    }
    IHS_FrameStatsSettleTracked(c->agg, c->tracker, 0); /* never expire OPEN in this schedule */
}
static void *report_frames(void *context) {
    concurrent_capture *c = context;
    IHS_FrameStatsMessage *message = calloc(1, sizeof(*message));
    assert(message);
    while (!atomic_load(&c->published)) {
        emit_report(c, message);
        sched_yield();
    }
    emit_report(c, message);
    free(message);
    return NULL;
}
static void simultaneous_receive_publish_report(void) {
    concurrent_capture c = {.lock = PTHREAD_MUTEX_INITIALIZER, .changed = PTHREAD_COND_INITIALIZER};
    atomic_init(&c.published, false);
    c.agg = IHS_FrameStatsAggregatorCreate();
    c.tracker = IHS_FrameTrackerCreate(4);
    assert(c.agg && c.tracker && IHS_FrameTrackerOpenEpoch(c.tracker, 1));
    pthread_t publisher, reporter;
    assert(pthread_create(&publisher, NULL, publish_frames, &c) == 0);
    assert(pthread_create(&reporter, NULL, report_frames, &c) == 0);
    for (unsigned id = 1; id <= 1000; ++id) {
        IHS_FrameStatsCollectTracked(c.agg, c.tracker);
        IHS_FrameStatsRecordReceived(c.agg, id, 100 + id, 200 + id, 300 + id, 100, 0);
        IHS_FrameReceive info = {0};
        IHS_FrameTicket *r;
        IHS_FrameBeginResult result;
        while ((result = IHS_FrameTrackerBegin(c.tracker, 1, id, &info, &r)) ==
               IHS_FrameBeginCapacity) {
            IHS_FrameStatsSettleTracked(c.agg, c.tracker, 0);
            sched_yield();
        }
        assert(result == IHS_FrameBeginOK);
        pthread_mutex_lock(&c.lock);
        while (c.count == 16)
            pthread_cond_wait(&c.changed, &c.lock);
        c.jobs[(c.head + c.count++) % 16] = r;
        pthread_cond_broadcast(&c.changed);
        pthread_mutex_unlock(&c.lock);
    }
    pthread_mutex_lock(&c.lock);
    c.inputDone = true;
    pthread_cond_broadcast(&c.changed);
    pthread_mutex_unlock(&c.lock);
    assert(pthread_join(publisher, NULL) == 0 && pthread_join(reporter, NULL) == 0);
    assert(c.agg->lastDisplayedFrameId == 1000 && c.agg->lastSentFrameId == 1000);
    IHS_FrameTrackerClose(c.tracker);
    IHS_FrameStatsAggregatorDestroy(c.agg);
    pthread_cond_destroy(&c.changed);
    pthread_mutex_destroy(&c.lock);
}
int main(void) {
    IHS_Init();
    bounded_no_display();
    cancellation_is_not_resource_expiry();
    publish_between_collection_and_cleanup();
    simultaneous_receive_publish_report();
    IHS_Quit();
    return 0;
}
