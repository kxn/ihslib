/* Exercise the real video worker and timer registration, including failures
 * after application start. Link wrappers affect only this test executable. */
#include <assert.h>
#include "test_session.h"
#include "ihslib.h"
#include "ihs_timer.h"
#include "session/session_pri.h"
#include "session/channels/video/ch_data_video.h"
#include "session/channels/channel.h"

enum mode { SUCCESS, APP_FAIL, TIMER_FAIL, SEND_FAIL, NO_START };
typedef struct {
    enum mode mode;
    int starts, stops, timers, cancels, sends;
    IHS_TimerTask *timer;
} fixture;
static _Thread_local fixture *starting;

IHS_TimerTask *__real_IHS_TimerTaskStart(IHS_Timer *, IHS_TimerRunFunction *,
                                        IHS_TimerEndFunction *, uint64_t, void *);
void __real_IHS_TimerTaskStopImmediate(IHS_TimerTask *);
bool __real_IHS_SessionSendControlMessage(IHS_Session *, EStreamControlMessage, const ProtobufCMessage *);
IHS_TimerTask *__wrap_IHS_TimerTaskStart(IHS_Timer *timer, IHS_TimerRunFunction *run,
                                        IHS_TimerEndFunction *end, uint64_t timeout, void *context) {
    if (starting && timeout == 1000) {
        if (starting->mode == TIMER_FAIL) return NULL;
        IHS_TimerTask *task = __real_IHS_TimerTaskStart(timer, run, end, timeout, context);
        assert(task);
        starting->timer = task;
        ++starting->timers;
        return task;
    }
    return __real_IHS_TimerTaskStart(timer, run, end, timeout, context);
}
void __wrap_IHS_TimerTaskStopImmediate(IHS_TimerTask *task) {
    bool tracked = starting && task == starting->timer;
    __real_IHS_TimerTaskStopImmediate(task);
    if (tracked) {
        ++starting->cancels;
        starting->timer = NULL;
    }
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
    assert(!f->timer && f->timers == f->cancels); /* timer stopped BEFORE app cleanup */
    ++f->stops;
    starting = NULL;
}
static void check(enum mode mode) {
    fixture f = {.mode = mode};
    IHS_Session *session = IHS_TestSessionCreate();
    assert(session);
    IHS_StreamVideoCallbacks callbacks = {.start = mode == NO_START ? NULL : on_start, .stop = on_stop};
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
    assert(f.timers == (mode == SUCCESS || mode == SEND_FAIL));
    assert(f.cancels == f.timers && !f.timer);
    assert(f.sends == (mode == SUCCESS || mode == SEND_FAIL));
    IHS_SessionDestroy(session);
}
int main(void) {
    IHS_Init();
    for (int i = SUCCESS; i <= NO_START; ++i) check((enum mode)i);
    IHS_Quit();
    return 0;
}
