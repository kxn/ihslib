/* Real CRC RX -> media worker -> production video consumer -> live collector,
 * concurrently read by snapshots. Encoded-frame sink stands in for a decoder;
 * all workers/readers join before session storage is released. Expected video
 * totals/bits come from official video_3_fragment_0, not a second C model. */
#include "ihslib.h"
#include "session/channels/video/ch_data_video.h"
#include "session/session_pri.h"
#include "ihs_buffer_ext.h"
#include "test_session.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>

static IHS_Session *session;
static atomic_uint tick, submitted;
static atomic_bool started, done;
uint32_t __wrap_IHS_SessionPacketTimestamp(void) { return atomic_load(&tick); }
bool __real_IHS_SessionSendControlMessage(IHS_Session *, EStreamControlMessage,
                                          const ProtobufCMessage *);
bool __wrap_IHS_SessionSendControlMessage(IHS_Session *s, EStreamControlMessage type,
                                          const ProtobufCMessage *m) {
    if (type == k_EStreamControlVideoDecoderInfo) {
        atomic_store(&started, true);
        return true;
    }
    return __real_IHS_SessionSendControlMessage(s, type, m);
}
static int start(IHS_Session *s, const IHS_StreamVideoConfig *c, void *ctx) {
    (void)s; (void)c; (void)ctx; return 0;
}
static IHS_StreamVideoSubmitResult submit(IHS_Session *s, uint16_t id, IHS_Buffer *b,
                                           IHS_StreamVideoFrameFlag flags, void *ctx) {
    (void)s; (void)flags; (void)ctx;
    assert(id >= 1 && id <= 13 && b->size == 20);
    atomic_fetch_add(&submitted, 1);
    return IHS_StreamVideoSubmitOK;
}
static void *read_samples(void *unused) {
    (void)unused;
    do {
        float values[5];
        IHS_SessionGetNetworkSample(session, values);
        assert(values[3] >= 0);
        sched_yield();
    } while (!atomic_load(&done));
    return NULL;
}
static void receive(unsigned id, unsigned fragment, uint32_t sent, uint32_t received,
                      const uint8_t *data, size_t size) {
    atomic_store(&tick, received);
    IHS_SessionPacket p = {.header = {.type = fragment ? IHS_SessionPacketTypeUnreliable : IHS_SessionPacketTypeUnreliableFrag,
        .hasCrc = true, .srcConnectionId = 34, .dstConnectionId = 12,
        .channelId = 3, .packetId = id, .fragmentId = fragment, .sendTimestamp = sent}};
    IHS_SessionPacketBodyInitialize(&p.body, true);
    IHS_BufferAppendMem(&p.body, data, size);
    IHS_SessionPacketPopulateBuffer(&p);
    IHS_Buffer wire = IHS_BUFFER_INIT(2048, 2048);
    IHS_BufferAppendMem(&wire, p.body.data, IHS_SessionPacketSize(&p));
    session->base.callbacks.received(&session->base, &session->info.address, &wire);
    IHS_BufferClear(&wire, true);
    IHS_SessionPacketClear(&p, true);
}
static unsigned bits(float value) { unsigned out; memcpy(&out, &value, sizeof(out)); return out; }
int main(void) {
    atomic_init(&tick, 900000); atomic_init(&submitted, 0);
    atomic_init(&started, false); atomic_init(&done, false);
    IHS_Init();
    session = IHS_TestSessionCreate();
    IHS_TimerTaskStopImmediate(session->retransmission.timer);
    session->state.connectionState = IHS_SessionConnectionStateConnected;
    session->state.connectionId = 12; session->state.hostConnectionId = 34;
    session->state.crcRequired = true;
    const IHS_StreamVideoCallbacks callbacks = {.start = start, .submit = submit};
    IHS_SessionSetVideoCallbacks(session, &callbacks, NULL);
    CStartVideoDataMsg config = CSTART_VIDEO_DATA_MSG__INIT;
    config.channel = 3; config.width = 1280; config.height = 720;
    config.codec = k_EStreamVideoCodecH264;
    IHS_SessionChannel *channel = IHS_SessionChannelDataVideoCreate(session, &config);
    assert(channel && !channel->initFailed);
    IHS_SessionChannelAdd(session, channel);
    uint64_t until = IHS_TimerNow() + 1000;
    while (!atomic_load(&started) && IHS_TimerNow() < until) usleep(1000);
    assert(atomic_load(&started));
    pthread_t reader;
    assert(!pthread_create(&reader, NULL, read_samples, NULL));
    for (unsigned fid = 1; fid <= 13; ++fid) {
        uint32_t sent = 900000 + fid * 1000;
        IHS_Buffer payload = IHS_BUFFER_INIT(64, 64);
        IHS_BufferAppendUInt8(&payload, k_EStreamDataPacket);
        IHS_BufferAppendUInt16LE(&payload, fid);
        IHS_BufferAppendUInt32LE(&payload, sent - 1);
        IHS_BufferAppendUInt16LE(&payload, 0);
        IHS_BufferAppendUInt32LE(&payload, 0);
        IHS_BufferAppendUInt16LE(&payload, 0);
        IHS_BufferAppendUInt8(&payload, VideoFrameFlagKeyFrame | VideoFrameFlagFrameFinish);
        IHS_BufferAppendUInt16LE(&payload, 0); IHS_BufferAppendUInt16LE(&payload, 0);
        for (unsigned n = 0; n < 20; ++n) IHS_BufferAppendUInt8(&payload, fid);
        receive((fid - 1) * 2, 1, sent, sent + 10, IHS_BufferPointer(&payload), 16);
        receive((fid - 1) * 2 + 1, 0, sent, sent + 20, IHS_BufferPointer(&payload) + 16, payload.size - 16);
        IHS_BufferClear(&payload, true);
        until = IHS_TimerNow() + 1000;
        while (atomic_load(&submitted) < fid && IHS_TimerNow() < until) usleep(1000);
        assert(atomic_load(&submitted) == fid);
    }
    atomic_store(&done, true);
    assert(!pthread_join(reader, NULL));
    IHS_BaseLock(&session->base);
    const IHS_NetworkVideoStats *v = &session->networkVideo;
    assert(v->frame == 13 && v->queue.count == 12 && v->frameBytes == 27);
    assert(v->totals[0] == 120 && v->totals[1] == 240 && v->totals[2] == 11760 && v->totals[3] == 324);
    assert(bits(v->bitrate) == 1180512027 && bits(v->link) == 1236061389);
    IHS_BaseUnlock(&session->base);
    IHS_SessionDestroy(session);
    IHS_Quit();
    return 0;
}
