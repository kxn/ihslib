/* Run the channel's real report callback with a controlled clock schedule;
 * serialize and unpack every attempted wire message at the queue boundary. */
#include "ihs_timer.h"
#include "ihslib.h"
#include "session/channels/channel.h"
#include "session/channels/video/ch_data_video.h"
#include "session/frame_stats.h"
#include "session/session_pri.h"
#include "test_session.h"
#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ready = PTHREAD_COND_INITIALIZER;
static bool started;
static _Thread_local bool videoWorker;
static IHS_TimerRunFunction *reportRun;
static void *reportContext;
static unsigned sends;
static IHS_Session *activeSession;

IHS_TimerTask *__real_IHS_TimerTaskStart(IHS_Timer *, IHS_TimerRunFunction *,
                                         IHS_TimerEndFunction *, uint64_t, void *);
bool __real_IHS_SessionSendControlMessage(IHS_Session *, EStreamControlMessage,
                                          const ProtobufCMessage *);
bool __real_IHS_SessionChannelStatsSend(IHS_SessionChannel *, EStreamStatsMessage,
                                        const ProtobufCMessage *, int32_t);
IHS_TimerTask *__wrap_IHS_TimerTaskStart(IHS_Timer *timer, IHS_TimerRunFunction *run,
                                         IHS_TimerEndFunction *end, uint64_t timeout,
                                         void *context) {
    if (videoWorker && timeout == 1000) {
        reportRun = run;
        reportContext = context;
        timeout = 60000; /* test drives report synchronously; CTest timeout < 60 s */
    }
    return __real_IHS_TimerTaskStart(timer, run, end, timeout, context);
}
bool __wrap_IHS_SessionSendControlMessage(IHS_Session *session, EStreamControlMessage type,
                                          const ProtobufCMessage *message) {
    if (type == k_EStreamControlVideoDecoderInfo) {
        pthread_mutex_lock(&lock);
        started = true;
        pthread_cond_signal(&ready);
        pthread_mutex_unlock(&lock);
        return true;
    }
    return __real_IHS_SessionSendControlMessage(session, type, message);
}
static void receive_frame(unsigned id) {
    IHS_FrameStatsAggregator *agg = activeSession->frameStats;
    IHS_FrameStatsRecordReceived(agg, id, 10, 20, 30, 100 + id, 1);
    IHS_FrameStatsRecordStage(agg, id, IHS_VideoFrameStageDecodeBegin, 40);
    IHS_FrameStatsRecordStage(agg, id, IHS_VideoFrameStageDecodeEnd, 50);
    IHS_FrameStatsRecordComplete(agg, id, IHS_VideoFrameResultDisplayed);
}
static void *receive_while_sending(void *unused) {
    (void)unused;
    receive_frame(2);
    /* Inject a next-interval sample while the previous report is borrowed.
     * This tests vector ownership, not the full-mode sampling contract. */
    IHS_MutexLock(activeSession->frameStats->lock);
    assert(IHS_FrameStatsAccumulatorAdd(&activeSession->frameStats->accumulator, 6, 42));
    IHS_MutexUnlock(activeSession->frameStats->lock);
    return NULL;
}
bool __wrap_IHS_SessionChannelStatsSend(IHS_SessionChannel *channel, EStreamStatsMessage type,
                                        const ProtobufCMessage *message, int32_t packetId) {
    if (type != k_EStreamStatsFrameEvents)
        return __real_IHS_SessionChannelStatsSend(channel, type, message, packetId);
    size_t size = protobuf_c_message_get_packed_size(message);
    uint8_t *packet = malloc(size);
    assert(packet);
    assert(protobuf_c_message_pack(message, packet) == size);
    CFrameStatsListMsg *decoded = cframe_stats_list_msg__unpack(NULL, size, packet);
    assert(decoded && decoded->n_stats == 1);
    ++sends;
    assert(sends <= 3);
    assert(decoded->latest_frame_id == sends && decoded->stats[0]->frame_id == sends);
    assert(decoded->stats[0]->n_events >= 3 && !decoded->stats[0]->has_frame_size);
    assert(activeSession->frameStats->lastSentFrameId == sends); /* retired BEFORE send */
    if (sends == 1) {
        pthread_t worker;
        assert(pthread_create(&worker, NULL, receive_while_sending, NULL) == 0);
        assert(pthread_join(worker, NULL) == 0);
    } else if (sends == 2) {
        bool found = false;
        for (size_t i = 0; i < decoded->n_accumulated_stats; ++i) {
            CFrameStatAccumulatedValue *row = decoded->accumulated_stats[i];
            if (row->stat_type == 6) {
                assert(row->count == 1 && row->average == 42);
                found = true;
            }
        }
        assert(found); /* finishing rejected report1 must not free next samples */
    }
    free(packet);
    cframe_stats_list_msg__free_unpacked(decoded, NULL);
    return sends == 2; /* Rejection retires report1/3; neither is replayed. */
}

static int on_start(IHS_Session *session, const IHS_StreamVideoConfig *config, void *context) {
    (void)session;
    (void)config;
    (void)context;
    videoWorker = true;
    return 0;
}
static void on_stop(IHS_Session *session, void *context) {
    (void)session;
    (void)context;
}
int main(void) {
    IHS_Init();
    activeSession = IHS_TestSessionCreate();
    assert(activeSession && activeSession->frameStats);
    IHS_FrameStatsAggregatorSetFullReporting(activeSession->frameStats, true);
    IHS_StreamVideoCallbacks callbacks = {.start = on_start, .stop = on_stop};
    IHS_SessionSetVideoCallbacks(activeSession, &callbacks, NULL);
    CStartVideoDataMsg msg = CSTART_VIDEO_DATA_MSG__INIT;
    msg.channel = 3;
    msg.codec = k_EStreamVideoCodecH264;
    IHS_SessionChannel *channel = IHS_SessionChannelDataVideoCreate(activeSession, &msg);
    assert(channel);
    pthread_mutex_lock(&lock);
    while (!started)
        pthread_cond_wait(&ready, &lock);
    pthread_mutex_unlock(&lock);
    assert(reportRun);
    receive_frame(1);
    assert(reportRun(0, reportContext) == 1000 && sends == 1);
    assert(activeSession->frameStats->lastQueuedFrameId == 0);
    assert(!activeSession->frameStats->reportPending);
    assert(reportRun(1, reportContext) == 1000 && sends == 2);
    assert(activeSession->frameStats->lastQueuedFrameId == 2);
    assert(reportRun(2, reportContext) == 1000 && sends == 2);
    receive_frame(3);
    assert(reportRun(3, reportContext) == 1000 && sends == 3);
    assert(!activeSession->frameStats->reportPending);
    assert(reportRun(4, reportContext) == 1000 && sends == 3);
    /* Channel teardown also frees a captured, not-yet-attempted snapshot. */
    receive_frame(4);
    const IHS_FrameStatsReport *borrowed = IHS_FrameStatsReportBegin(activeSession->frameStats);
    assert(borrowed && borrowed->count == 1);
    uint64_t abandonedSerial = borrowed->serial;
    IHS_SessionChannelStop(channel);
    IHS_SessionChannelDestroy(channel);
    assert(!activeSession->frameStats->reportPending);
    assert(activeSession->frameStats->closedUnsentFrames == 1);
    assert(!IHS_FrameStatsReportFinish(activeSession->frameStats, abandonedSerial, true));
    assert(!IHS_FrameStatsReportBegin(activeSession->frameStats));
    IHS_SessionDestroy(activeSession);
    IHS_Quit();
    pthread_cond_destroy(&ready);
    pthread_mutex_destroy(&lock);
    return 0;
}
