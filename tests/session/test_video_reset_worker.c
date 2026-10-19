/* Native concurrency/lifetime verification of the official §79/80 contract.
 * Actual control FIFO, video worker/assembly/tickets and receive-cycle run.
 * Platform decoder and socket readiness are controlled boundaries. Nonempty
 * window/FEC preservation is a local ownership check, not new Source evidence. */
#include "ihs_buffer.h"
#include "ihs_buffer_ext.h"
#include "ihslib.h"
#include "ihslib/frame_ticket.h"
#include "session/channels/ch_control.h"
#include "session/channels/ch_data.h"
#include "session/channels/video/ch_data_video.h"
#include "session/session_pri.h"
#include "session/unreliable.h"
#include "test_session.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>

static pthread_t owner;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static bool blockFlush, entered, joining, released, failWorker, cancelOnStop, lateControl;
static unsigned prepares, starts, stops, infos, lost, waits, submissions;
static unsigned order, joinedAt, stoppedAt, startedAt, lostAt, waitAt;
static unsigned unavailableStarts;
static atomic_uint processed;
#include "decoder_think_observer.h"
static IHS_Thread *worker;
static IHS_VideoEpochInfo active;
static IHS_FrameTicket *held;
static IHS_SessionChannelData *video;
static void onOwner(void) {
    assert(pthread_equal(owner, pthread_self()));
}
uint32_t __wrap_IHS_SessionPacketTimestamp(void) {
    return 1000000;
}
int __wrap_IHS_UDPSocketWaitReadable(IHS_UDPSocket *socket, unsigned timeout) {
    (void)socket;
    (void)timeout;
    onOwner();
    ++waits;
    waitAt = ++order;
    return 0;
}
bool __wrap_IHS_BaseSend(IHS_Base *b, IHS_SocketAddress a, const IHS_Buffer *p) {
    (void)b;
    (void)a;
    (void)p;
    return true;
}
IHS_Thread *__real_IHS_ThreadCreate(IHS_ThreadFunction *, const char *, void *);
IHS_Thread *__wrap_IHS_ThreadCreate(IHS_ThreadFunction *fn, const char *name, void *ctx) {
    if (strcmp(name, "IHSVideo"))
        return __real_IHS_ThreadCreate(fn, name, ctx);
    onOwner();
    assert(!worker);
    worker = failWorker ? NULL : __real_IHS_ThreadCreate(fn, name, ctx);
    return worker;
}
void __real_IHS_ThreadJoin(IHS_Thread *);
void __wrap_IHS_ThreadJoin(IHS_Thread *thread) {
    bool videoWorker = thread == worker;
    if (videoWorker) {
        onOwner();
        pthread_mutex_lock(&lock);
        joining = true;
        pthread_cond_broadcast(&cond);
        pthread_mutex_unlock(&lock);
    }
    __real_IHS_ThreadJoin(thread);
    if (videoWorker) {
        worker = NULL;
        joinedAt = ++order;
    }
}
void __real_IHS_SessionPacketsWindowReleaseFrame(IHS_SessionFrame *);
void __wrap_IHS_SessionPacketsWindowReleaseFrame(IHS_SessionFrame *f) {
    __real_IHS_SessionPacketsWindowReleaseFrame(f);
    assert(!pthread_equal(owner, pthread_self()));
    atomic_fetch_add(&decoderInputsConsumed, 1);
}
bool __real_IHS_SessionSendControlMessage(IHS_Session *, EStreamControlMessage,
                                          const ProtobufCMessage *);
bool __wrap_IHS_SessionSendControlMessage(IHS_Session *s, EStreamControlMessage t,
                                          const ProtobufCMessage *m) {
    if (t != k_EStreamControlVideoDecoderInfo)
        return __real_IHS_SessionSendControlMessage(s, t, m);
    onOwner();
    ++infos;
    return false; /* Failed Info send never rolls back a live decoder. */
}
void __real_IHS_SessionChannelDataLost(IHS_SessionChannel *);
void __wrap_IHS_SessionChannelDataLost(IHS_SessionChannel *ch) {
    onOwner();
    assert(!video || video == (void *)ch);
    ++lost;
    lostAt = ++order;
    __real_IHS_SessionChannelDataLost(ch);
}
static int prepare(IHS_Session *s, const IHS_StreamVideoConfig *c, void *ctx) {
    (void)s;
    (void)ctx;
    onOwner();
    ++prepares;
    assert(c->codec == IHS_StreamVideoCodecH264 && c->width == 1280 && c->height == 720);
    return 0;
}
static int start(IHS_Session *s, const IHS_VideoEpochInfo *e, const IHS_StreamVideoConfig *c,
                 void *ctx) {
    (void)s;
    (void)ctx;
    onOwner();
    assert(!worker);
    assert(c->codecDataLen == 4 && !memcmp(c->codecData, "abcd", 4));
    active = *e;
    ++starts;
    startedAt = ++order;
    return starts <= unavailableStarts ? 1 : 0;
}
static void stop(IHS_Session *s, const IHS_VideoEpochInfo *e, void *ctx) {
    (void)ctx;
    onOwner();
    assert(!worker && e->video_epoch == active.video_epoch);
    if (video)
        assert(!video->fecQueuedCount && !video->fecHead);
    ++stops;
    stoppedAt = ++order;
    if (cancelOnStop)
        IHS_SessionInterrupt(s);
}
static IHS_StreamVideoSubmitResult submit(IHS_Session *s, const IHS_VideoEpochInfo *e, uint16_t id,
                                          IHS_FrameTicket *ticket, IHS_Buffer *data,
                                          IHS_StreamVideoFrameFlag flags, bool *taken, void *ctx) {
    (void)s;
    (void)flags;
    (void)ctx;
    assert(!pthread_equal(owner, pthread_self()) && e->video_epoch == active.video_epoch);
    assert(data->size == 1 && *IHS_BufferPointer(data) == 'x');
    ++submissions;
    if (id == 3) {
        assert(!held && IHS_FrameTicketRetain(ticket));
        held = ticket;
        *taken = true;
    }
    return IHS_StreamVideoSubmitReset;
}
/* Pending requests gate submit, but key fragments still flush the old backend.
 * Block that real worker phase to verify reset joins before retiring resources. */
static void flush(IHS_Session *s, const IHS_VideoEpochInfo *e, IHS_StreamVideoFlushPhase phase,
                  void *ctx) {
    (void)s; (void)e; (void)ctx;
    if (phase != IHS_StreamVideoFlushDecoder || pthread_equal(owner, pthread_self()))
        return;
    pthread_mutex_lock(&lock);
    if (blockFlush) {
        entered = true;
        pthread_cond_broadcast(&cond);
        while (!released)
            pthread_cond_wait(&cond, &lock);
    }
    pthread_mutex_unlock(&lock);
}
static void feed(IHS_Session *s, uint16_t id, uint16_t seq, bool key) {
    IHS_SessionFrame f = {.header = {.type = IHS_SessionPacketTypeReliable,
                                     .channelId = 3,
                                     .sendTimestamp = 1000000,
                                     .receiveTimestamp = 1000000,
                                     .receiveBegin = 1000000}};
    IHS_BufferInit(&f.body, 64, 64);
    IHS_BufferAppendUInt8(&f.body, k_EStreamDataPacket);
    IHS_BufferAppendUInt16LE(&f.body, id);
    IHS_BufferAppendUInt32LE(&f.body, 1000000);
    IHS_BufferAppendUInt16LE(&f.body, 0);
    IHS_BufferAppendUInt32LE(&f.body, 0);
    IHS_BufferAppendUInt16LE(&f.body, seq);
    IHS_BufferAppendUInt8(&f.body, VideoFrameFlagFrameFinish | (key ? VideoFrameFlagKeyFrame : 0));
    IHS_BufferAppendUInt16LE(&f.body, 0);
    IHS_BufferAppendUInt16LE(&f.body, 0);
    IHS_BufferAppendUInt8(&f.body, 'x');
    IHS_SessionDeliverReliableFrame(s, &f);
    IHS_SessionFrameClear(&f, true);
}
static void waitProcessed(unsigned count) {
    for (unsigned i = 0; i < 5000 && atomic_load(&processed) < count; ++i)
        usleep(1000);
    assert(atomic_load(&processed) == count);
}
void __real_IHS_SessionChannelControlOnMessageReceived(IHS_SessionChannel *, EStreamControlMessage,
                                                       IHS_Buffer *,
                                                       const IHS_SessionPacketHeader *);
void __wrap_IHS_SessionChannelControlOnMessageReceived(IHS_SessionChannel *ch,
                                                       EStreamControlMessage t, IHS_Buffer *b,
                                                       const IHS_SessionPacketHeader *h) {
    __real_IHS_SessionChannelControlOnMessageReceived(ch, t, b, h);
    if (t == k_EStreamControlVideoEncoderInfo && lateControl) {
        lateControl = false;
        feed(ch->session, 3, 2, true);
        waitProcessed(3); /* Actual worker publishes request during this cycle's control drain. */
    }
}
static void cycle(IHS_Session *s) {
    IHS_UDPPacket packet = {0};
    assert(s->base.callbacks.run->receiveCycle(&s->base, s->base.callbackContexts.run, &packet));
    IHS_BufferClear(&packet.buffer, true);
}
static void control(IHS_Session *s, EStreamControlMessage t, const ProtobufCMessage *m) {
    IHS_Buffer b = IHS_BUFFER_INIT(64, 4096);
    IHS_BufferAppendMessage(&b, m);
    IHS_SessionPacketHeader h = {.type = IHS_SessionPacketTypeReliable, .channelId = 1};
    IHS_SessionChannelControlHandoff(s->channels[1], t, &b, &h);
    IHS_SessionChannelControlDrainIncoming(s->channels[1]);
    IHS_BufferClear(&b, true);
}
static void *releaseDuringJoin(void *ctx) {
    (void)ctx;
    pthread_mutex_lock(&lock);
    while (!joining)
        pthread_cond_wait(&cond, &lock);
    assert(entered && !released);
    released = true;
    pthread_cond_broadcast(&cond);
    pthread_mutex_unlock(&lock);
    return NULL;
}
static bool fecPiece(IHS_FECReceiver *r, unsigned index, IHS_SessionFrame *out) {
    IHS_SessionPacket p = {
        .header = {.type = IHS_SessionPacketTypeUnreliableFEC, .packetId = 0, .fragmentId = index}};
    IHS_BufferInit(&p.body, 16, 16);
    IHS_BufferAppendUInt32LE(&p.body, 4);
    IHS_BufferAppendUInt16LE(&p.body, 2);
    IHS_BufferAppendUInt16LE(&p.body, 0);
    IHS_BufferAppendMem(&p.body, index ? (const uint8_t *)"cd" : (const uint8_t *)"ab", 2);
    bool ready = IHS_FECReceiverAdd(r, &p, out);
    IHS_BufferClear(&p.body, true);
    return ready;
}
int main(void) {
    owner = pthread_self();
    IHS_Init();
    IHS_Session *s = IHS_TestSessionCreate();
    assert(s);
    IHS_TimerTaskStopImmediate(s->retransmission.timer);
    s->state.connectionState = IHS_SessionConnectionStateConnected;
    s->negotiatedVideoCodec = k_EStreamVideoCodecH264;
    assert(IHS_SessionSetVideoTrackingIdentity(s, 90));
    IHS_StreamVideoCallbacks cb = {
        .prepare = prepare, .startTracked = start, .submitTracked = submit, .stopTracked = stop,
        .flushTracked = flush};
    IHS_SessionSetVideoCallbacks(s, &cb, NULL);
    unavailableStarts = 1;
    CStartVideoDataMsg m = CSTART_VIDEO_DATA_MSG__INIT;
    m.channel = 3;
    m.has_width = m.has_height = m.has_codec_data = true;
    m.width = 1280;
    m.height = 720;
    m.codec_data.data = (uint8_t *)"abcd";
    m.codec_data.len = 4; /* codec absent: negotiated fallback */
    control(s, k_EStreamControlStartVideoData, &m.base);
    video = (void *)IHS_SessionChannelForType(s, IHS_SessionChannelTypeDataVideo);
    assert(video && worker && starts == 1 && prepares == 1 && infos == 0 &&
           active.video_epoch == 1);
    feed(s, 1, 0, true);
    waitProcessed(1);
    assert(!lost && !submissions && stops == 0); /* Deferred reset, no backend call. */
    cycle(s);
    assert(stops == 1 && starts == 2 && prepares == 1 && infos == 1 && lost == 1 &&
           active.video_epoch == 2);
    assert(joinedAt < stoppedAt && stoppedAt < startedAt && startedAt < lostAt && lostAt < waitAt);
    feed(s, 2, 1, false);
    waitProcessed(2);
    assert(!submissions); /* Strict gate after reset. */
    CVideoEncoderInfoMsg info = CVIDEO_ENCODER_INFO_MSG__INIT;
    info.info = "fixture";
    IHS_Buffer infoBody = IHS_BUFFER_INIT(64, 4096);
    IHS_BufferAppendMessage(&infoBody, &info.base);
    IHS_SessionPacketHeader infoHeader = {.type = IHS_SessionPacketTypeReliable, .channelId = 1};
    IHS_SessionChannelControlHandoff(s->channels[1], k_EStreamControlVideoEncoderInfo, &infoBody,
                                     &infoHeader);
    IHS_BufferClear(&infoBody, true);
    lateControl = true;
    cycle(s); /* Request made after the cycle's reset gate waits for the next cycle. */
    assert(!lateControl && starts == 2 && lost == 1);
    assert(submissions == 1 && held);
    /* Continuous data is consumed but every pending request gates FinalDecode. */
    feed(s, 4, 3, false);
    waitProcessed(4);
    assert(submissions == 1);
    blockFlush = true;
    feed(s, 5, 4, true);
    pthread_mutex_lock(&lock);
    while (!entered)
        pthread_cond_wait(&cond, &lock);
    joining = false;
    pthread_mutex_unlock(&lock);
    feed(s, 6, 5, true);
    feed(s, 7, 6, true);
    IHS_MutexLock(video->windowLock);
    assert(video->fecQueuedCount == 2);
    IHS_MutexUnlock(video->windowLock);
    IHS_UnreliableChannel *rx = video->receive;
    IHS_SessionPacket p = {.header = {.type = IHS_SessionPacketTypeUnreliableFrag,
                                      .packetId = 1,
                                      .fragmentId = 1,
                                      .sendTimestamp = 1000000,
                                      .receiveTimestamp = 1000000}};
    IHS_BufferInit(&p.body, 8, 8);
    IHS_BufferAppendUInt8(&p.body, 42);
    assert(IHS_SessionPacketsWindowAdd(rx->window, &p));
    IHS_SessionPacketClear(&p, true);
    uint16_t slots = IHS_SessionPacketsWindowSize(rx->window);
    IHS_SessionFrame recovered = {0};
    assert(!fecPiece(rx->fec, 0, &recovered));
    pthread_t releaser;
    assert(!pthread_create(&releaser, NULL, releaseDuringJoin, NULL));
    cycle(s);
    assert(!pthread_join(releaser, NULL));
    /* Stop interrupted the decoder before its next OnThink, but joined the
     * current input release. This is consumption, not a submitted image. */
    assert(atomic_load(&decoderInputsConsumed) == 5);
    assert(submissions == 1 && stops == 2 && starts == 3 && prepares == 1 && lost == 2);
    assert(active.video_epoch == 3 && video->receive == rx &&
           slots == IHS_SessionPacketsWindowSize(rx->window));
    assert(IHS_SessionPacketsWindowNextNeededPacketId(rx->window) == 0);
    assert(fecPiece(rx->fec, 1, &recovered) && recovered.body.size == 4 &&
           !memcmp(IHS_BufferPointer(&recovered.body), "abcd", 4));
    IHS_SessionFrameClear(&recovered, true);
    assert(joinedAt < stoppedAt && stoppedAt < startedAt && startedAt < lostAt && lostAt < waitAt);
    cycle(s);
    assert(lost == 2 && starts == 3); /* Old requests coalesced; no repeat drain. */
    assert(IHS_FrameTicketIdentity(held).epoch == 2);
    IHS_FrameOutcome outcome = {.result = IHS_VideoFrameResultDisplayed, .presentationSerial = 1};
    IHS_FrameTicketComplete(held, &outcome);
    IHS_FrameTicketRelease(held);
    held = NULL;
    blockFlush = false;
    feed(s, 8, 7, true);
    waitProcessed(6);
    failWorker = true;
    cycle(s); /* OS Start failure retains actor and bounded queue. */
    assert(!worker && starts == 4 && infos == 3 && lost == 3 && !video->interrupted);
    feed(s, 9, 8, true);
    assert(video->fecQueuedCount == 1);
    CStopVideoDataMsg stopMsg = CSTOP_VIDEO_DATA_MSG__INIT;
    control(s, k_EStreamControlStopVideoData, &stopMsg.base);
    video = NULL;
    assert(stops == 4 && !worker);
    /* Cancellation in stop excludes the replacement backend and worker. */
    failWorker = false;
    control(s, k_EStreamControlStartVideoData, &m.base);
    video = (void *)IHS_SessionChannelForType(s, IHS_SessionChannelTypeDataVideo);
    feed(s, 10, 0, true);
    waitProcessed(7);
    cancelOnStop = true;
    s->base.callbacks.run->update(&s->base, s->base.callbackContexts.run);
    assert(s->base.interrupted && starts == 5 && stops == 5 && lost == 3 && !worker);
    IHS_SessionDestroy(s);
    IHS_Quit();
    return 0;
}
