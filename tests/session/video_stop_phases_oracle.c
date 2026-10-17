/* Include the production TU to observe private assembly/clock state, without a
 * test-only production API or a second state-machine implementation. Worker,
 * tracker, assembly, actor Reset/Stop and queued DataLost are production code.
 * Platform player/backend phases and their elapsed ticks are recording inputs. */
#include "../../src/session/channels/video/ch_data_video.c"
#include "ihs_buffer_ext.h"
#include "test_session.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static uint32_t tick, flushDelay, deleteDelay, initDelay;
static bool oldBackend, readyBackend, haveBackend, marking, marked;
static unsigned starts, lost;
static IHS_SessionChannelVideo *video;
static const char *phaseNames[4];
static uint32_t phaseTicks[4];
static unsigned phaseCount;
static void phase(const char *name) {
    if (marking) {
        assert(phaseCount < 4);
        phaseNames[phaseCount] = name;
        phaseTicks[phaseCount++] = tick;
    }
}
uint32_t __wrap_IHS_SessionPacketTimestamp(void) {
    if (marking && !marked) {
        marked = true;
        phase("mark");
    }
    return tick;
}
bool __wrap_IHS_SessionSendControlMessage(IHS_Session *s, EStreamControlMessage type,
                                          const ProtobufCMessage *msg) {
    (void)s;
    (void)type;
    (void)msg;
    return true;
}
void __wrap_IHS_SessionChannelDataLost(IHS_SessionChannel *ch) {
    (void)ch;
    ++lost;
}
static int prepare(IHS_Session *s, const IHS_StreamVideoConfig *cfg, void *ctx) {
    (void)s;
    (void)cfg;
    (void)ctx;
    return 0;
}
static int start(IHS_Session *s, const IHS_VideoEpochInfo *e, const IHS_StreamVideoConfig *cfg,
                 void *ctx) {
    (void)s;
    (void)e;
    (void)cfg;
    (void)ctx;
    haveBackend = ++starts == 1 ? oldBackend : readyBackend;
    if (starts > 1)
        tick += initDelay;
    return haveBackend ? 0 : 1;
}
static void flush(IHS_Session *s, const IHS_VideoEpochInfo *e, IHS_StreamVideoFlushPhase which,
                  void *ctx) {
    (void)s;
    (void)e;
    (void)ctx;
    if (which == IHS_StreamVideoFlushPlayer) {
        phase("player");
        assert(video && video->base.interrupted);
        if (marking)
            assert(video->frame.buffer.size || video->frame.partial.head);
    } else {
        assert(!video->frame.buffer.size && !video->frame.partial.head);
        if (marking && haveBackend)
            tick += flushDelay;
        phase("decoder");
    }
}
static void stop(IHS_Session *s, const IHS_VideoEpochInfo *e, void *ctx) {
    (void)s;
    (void)e;
    (void)ctx;
    if (haveBackend)
        tick += deleteDelay;
    haveBackend = false;
    phase("retire");
}
static IHS_StreamVideoSubmitResult submit(IHS_Session *s, const IHS_VideoEpochInfo *e, uint16_t id,
                                          IHS_FrameTicket *t, IHS_Buffer *data,
                                          IHS_StreamVideoFrameFlag flags, bool *taken, void *ctx) {
    (void)s;
    (void)e;
    (void)id;
    (void)t;
    (void)data;
    (void)flags;
    (void)taken;
    (void)ctx;
    return IHS_StreamVideoSubmitReset;
}
static void feed(IHS_SessionChannel *ch, uint16_t id, uint16_t seq, uint8_t flags) {
    IHS_Buffer body = IHS_BUFFER_INIT(64, 64);
    IHS_BufferAppendUInt16LE(&body, seq);
    IHS_BufferAppendUInt8(&body, flags);
    IHS_BufferAppendUInt16LE(&body, 0);
    IHS_BufferAppendUInt16LE(&body, 0);
    IHS_BufferAppendMem(&body, (const uint8_t *)"partial-packet", 14);
    IHS_SessionDataFrameHeader h = {.id = id, .timestamp = 1000000};
    DataReceived(ch, &h, &body);
    IHS_BufferClear(&body, true);
}
int main(int argc, char **argv) {
    assert(argc == 9);
    tick = strtoul(argv[1], NULL, 10);
    bool reset = atoi(argv[2]);
    oldBackend = atoi(argv[3]);
    readyBackend = atoi(argv[4]);
    flushDelay = strtoul(argv[5], NULL, 10);
    deleteDelay = strtoul(argv[6], NULL, 10);
    initDelay = strtoul(argv[7], NULL, 10);
    bool escape = atoi(argv[8]);
    IHS_Init();
    IHS_Session *s = IHS_TestSessionCreate();
    assert(s);
    IHS_TimerTaskStopImmediate(s->retransmission.timer);
    s->state.connectionState = IHS_SessionConnectionStateConnected;
    assert(IHS_SessionSetVideoTrackingIdentity(s, 90));
    IHS_StreamVideoCallbacks cb = {.prepare = prepare,
                                   .startTracked = start,
                                   .submitTracked = submit,
                                   .stopTracked = stop,
                                   .flushTracked = flush};
    IHS_SessionSetVideoCallbacks(s, &cb, NULL);
    CStartVideoDataMsg cfg = CSTART_VIDEO_DATA_MSG__INIT;
    cfg.channel = 3;
    cfg.codec = k_EStreamVideoCodecH264;
    cfg.width = 1280;
    cfg.height = 720;
    IHS_SessionChannel *ch = IHS_SessionChannelDataVideoCreate(s, &cfg);
    assert(ch);
    IHS_SessionChannelAdd(s, ch);
    video = (void *)ch;
    feed(ch, 1, 0, VideoFrameFlagKeyFrame | VideoFrameFlagFrameFinish);
    feed(ch, 30, 1, escape ? VideoFrameFlagNeedEscape : 0);
    feed(ch, 30, 2, escape ? VideoFrameFlagNeedEscape : 0);
    assert(video->frame.buffer.size || video->frame.partial.head);
    marking = true;
    if (reset)
        IHS_SessionVideoHandlePendingReset(s);
    else {
        IHS_SessionChannelDataJoinAndPurge(&video->base);
        DataStop(ch);
    }
    marking = false;
    assert(marked);
    uint32_t mark = video->states.lastRecoveryRequest;
    bool active = video->appStarted, backend = haveBackend;
    assert(!video->frame.buffer.size && !video->frame.partial.head);
    unsigned before = lost;
    if (reset)
        feed(ch, 31, 3, 0);
    printf("{\"active\":%s,\"backend\":%s,\"strict\":%u,\"soft\":%u,\"mark\":%u,\"tick\":%u,"
           "\"lost\":[%u,%u],\"phase_order\":[",
           active ? "true" : "false", backend ? "true" : "false", video->states.waitingKeyFrame,
           video->states.syncingBacklog, mark, tick, before, lost);
    assert(phaseCount == 4);
    for (unsigned n = 0; n < phaseCount; ++n)
        printf("%s[\"%s\",%u]", n ? "," : "", phaseNames[n], phaseTicks[n]);
    puts("]}");
    IHS_SessionDestroy(s);
    IHS_Quit();
    return 0;
}
