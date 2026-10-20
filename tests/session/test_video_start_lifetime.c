/* Real video worker rollback, plus session-owned stats allocation failures.
 * Video start must never register an independent report timer. */
#include "ihs_timer.h"
#include "ihslib.h"
#include "session/channels/channel.h"
#include "session/channels/video/ch_data_video.h"
#include "session/frame_stats.h"
#include "session/session_pri.h"
#include "test_session.h"
#include <assert.h>
#include <stdlib.h>

enum mode { SUCCESS, APP_FAIL, SEND_FAIL, NO_START };
typedef struct {
    enum mode mode;
    int starts, stops, sends;
} fixture;
static _Thread_local fixture *starting;
static _Thread_local unsigned allocation, fail_allocation;
static _Thread_local bool allocation_failed;
static bool fail_creation(void) {
    if (fail_allocation && ++allocation == fail_allocation) {
        allocation_failed = true;
        return true;
    }
    return false;
}
void *__real_malloc(size_t);
void *__wrap_malloc(size_t bytes) {
    return fail_creation() ? NULL : __real_malloc(bytes);
}

void *__real_calloc(size_t, size_t);
void *__wrap_calloc(size_t count, size_t size) {
    if (fail_creation())
        return NULL;
    return __real_calloc(count, size);
}

IHS_TimerTask *__real_IHS_TimerTaskStart(IHS_Timer *, IHS_TimerRunFunction *,
                                         IHS_TimerEndFunction *, uint64_t, void *);
bool __real_IHS_SessionSendControlMessage(IHS_Session *, EStreamControlMessage,
                                          const ProtobufCMessage *);
IHS_TimerTask *__wrap_IHS_TimerTaskStart(IHS_Timer *timer, IHS_TimerRunFunction *run,
                                         IHS_TimerEndFunction *end, uint64_t timeout,
                                         void *context) {
    assert(!starting || timeout != 1000);
    return __real_IHS_TimerTaskStart(timer, run, end, timeout, context);
}
bool __wrap_IHS_SessionSendControlMessage(IHS_Session *session, EStreamControlMessage type,
                                          const ProtobufCMessage *message) {
    if (type == k_EStreamControlVideoDecoderInfo) {
        assert(starting);
        ++starting->sends;
        return starting->mode != SEND_FAIL;
    }
    return __real_IHS_SessionSendControlMessage(session, type, message);
}
static int on_start(IHS_Session *session, const IHS_StreamVideoConfig *config, void *context) {
    (void)session;
    assert(config->width == 1280 && config->height == 720);
    fixture *f = context;
    starting = f;
    ++f->starts;
    return f->mode == APP_FAIL ? -1 : 0;
}
static void on_stop(IHS_Session *session, void *context) {
    (void)session;
    fixture *f = context;
    ++f->stops;
    starting = NULL;
}
static void check(enum mode mode) {
    fixture f = {.mode = mode};
    IHS_Session *session = IHS_TestSessionCreate();
    assert(session);
    IHS_StreamVideoCallbacks callbacks = {.start = mode == NO_START ? NULL : on_start,
                                          .stop = on_stop};
    IHS_SessionSetVideoCallbacks(session, &callbacks, &f);
    CStartVideoDataMsg msg = CSTART_VIDEO_DATA_MSG__INIT;
    msg.channel = 3;
    msg.codec = k_EStreamVideoCodecH264;
    msg.width = 1280;
    msg.height = 720;
    IHS_SessionChannel *channel = IHS_SessionChannelDataVideoCreate(session, &msg);
    assert(channel);
    /* Creation starts the real worker. Destroy joins it even when start fails;
     * no sleeps, session networking, or synthetic application callbacks. */
    IHS_SessionChannelStop(channel);
    IHS_SessionChannelStop(channel);
    IHS_SessionChannelDestroy(channel);
    assert(f.starts == (mode != NO_START));
    assert(f.stops == (mode != NO_START && mode != APP_FAIL));
    assert(f.sends == (mode == SUCCESS || mode == SEND_FAIL));
    IHS_SessionDestroy(session);
    starting = NULL;
}
static void construction_failures(void) {
    unsigned failures = 0;
    for (unsigned point = 1; point <= 12; ++point) {
        IHS_Session *session = IHS_TestSessionCreate();
        assert(session);
        CStartVideoDataMsg msg = CSTART_VIDEO_DATA_MSG__INIT;
        msg.channel = 3;
        msg.width = 1280;
        msg.height = 720;
        msg.codec = k_EStreamVideoCodecH264;
        unsigned char extra[4] = {0, 0, 0, 1};
        msg.has_codec_data = true;
        msg.codec_data.data = extra;
        msg.codec_data.len = sizeof(extra);
        allocation = 0;
        allocation_failed = false;
        fail_allocation = point;
        IHS_SessionChannel *channel = IHS_SessionChannelDataVideoCreate(session, &msg);
        fail_allocation = 0;
        if (allocation_failed) {
            assert(!channel);
            ++failures;
        }
        if (channel) {
            IHS_SessionChannelStop(channel);
            IHS_SessionChannelDestroy(channel);
        }
        IHS_SessionDestroy(session);
    }
    assert(failures >= 8); /* backing, locks, condition, window, worker allocation */
}
static void stats_construction_failures(void) {
    unsigned failures = 0;
    for (unsigned point = 1; point <= 6; ++point) {
        allocation = 0;
        allocation_failed = false;
        fail_allocation = point;
        IHS_FrameStatsAggregator *agg = IHS_FrameStatsAggregatorCreate();
        fail_allocation = 0;
        if (allocation_failed) {
            assert(!agg);
            ++failures;
        }
        IHS_FrameStatsAggregatorDestroy(agg);
    }
    assert(failures >= 4); /* aggregator/scratch, two locks, tracked scratch */
}
static void invalid_flush(IHS_Session *s, const IHS_VideoEpochInfo *e, IHS_StreamVideoFlushPhase p,
                          void *ctx) {
    (void)s;
    (void)e;
    (void)p;
    (void)ctx;
    assert(false);
}
static void invalid_interrupt(IHS_Session *s,const IHS_VideoEpochInfo *e,bool restart,void *ctx) {
    (void)s;(void)e;(void)restart;(void)ctx;assert(false);
}
static void invalid_think(IHS_Session *s,const IHS_VideoEpochInfo *e,void *ctx) {
    (void)s;(void)e;(void)ctx;assert(false);
}
static int invalid_start(IHS_Session *s,const IHS_VideoEpochInfo *e,
                          const IHS_StreamVideoConfig *config,void *ctx) {
    (void)s;(void)e;(void)config;(void)ctx;assert(false);return -1;
}
static void extensions_require_tracked_callbacks(unsigned mode) {
    IHS_Session *s = IHS_TestSessionCreate();
    assert(s);
    IHS_StreamVideoCallbacks cb = {0};
    if(mode==0) cb.flushTracked=invalid_flush;
    if(mode==1 || mode>=3) cb.thinkTracked=invalid_think;
    if(mode==2 || mode>=3) cb.interruptTracked=invalid_interrupt;
    if(mode==4) cb.startTracked=invalid_start;
    IHS_SessionSetVideoCallbacks(s, &cb, NULL);
    assert(s->invalidVideoCallbacks);
    CStartVideoDataMsg msg = CSTART_VIDEO_DATA_MSG__INIT;
    msg.channel = 3;
    msg.codec = k_EStreamVideoCodecH264;
    msg.width = 1280;
    msg.height = 720;
    IHS_SessionChannel *ch = IHS_SessionChannelDataVideoCreate(s, &msg);
    assert(s->base.interrupted);
    if (ch) {
        IHS_SessionChannelStop(ch);
        IHS_SessionChannelDestroy(ch);
    }
    IHS_SessionDestroy(s);
}
int main(void) {
    IHS_Init();
    for(unsigned mode=0;mode<5;++mode) extensions_require_tracked_callbacks(mode);
    stats_construction_failures();
    construction_failures();
    for (int i = SUCCESS; i <= NO_START; ++i)
        check((enum mode)i);
    IHS_Quit();
    return 0;
}
