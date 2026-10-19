/* Original prequeued batches drive the real FIFO and joinable decoder worker.
 * Initial worker startup is held at the OS factory, then the real pthread runs.
 * Infinite-wait observation lets the driver join/resume the same decoder between
 * batches without retiring its backend/pending state. SDK/player are terminals. */
#define IHS_SUBFRAME_OBSERVER 1
#define IHS_DECODER_EXTERNAL_THREAD_FACTORY 1
#define main                  queue_unused_content_main
#include "video_content_mode_oracle.c"
#undef main
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>

#ifndef IHS_VIDEO_QUEUE_OBSERVATIONS
#define IHS_VIDEO_QUEUE_OBSERVATIONS 100
#endif
static bool holdStartup = true;
static atomic_bool idle, inThink, releaseThink;
static bool wakeMode;
static unsigned waits[IHS_VIDEO_QUEUE_OBSERVATIONS][2], waitCount, consumed[IHS_VIDEO_QUEUE_OBSERVATIONS][2], consumedCount;

IHS_Thread *__real_IHS_ThreadCreate(IHS_ThreadFunction *, const char *, void *);
IHS_Thread *__wrap_IHS_ThreadCreate(IHS_ThreadFunction *fn, const char *name, void *ctx) {
    if (holdStartup && !strcmp(name, "IHSVideo"))
        return NULL;
    return __real_IHS_ThreadCreate(fn, name, ctx);
}
bool __real_IHS_CondWait(IHS_Cond *, IHS_Mutex *);
bool __real_IHS_CondWaitTimeout(IHS_Cond *, IHS_Mutex *, uint32_t);
static void recordWait(uint32_t ms) {
    assert(waitCount < IHS_VIDEO_QUEUE_OBSERVATIONS);
    waits[waitCount][0] = action;
    waits[waitCount++][1] = ms;
    if (ms == UINT32_MAX)
        atomic_store(&idle, true);
}
bool __wrap_IHS_CondWait(IHS_Cond *c, IHS_Mutex *m) {
    if (video && c == video->base.windowCond)
        recordWait(UINT32_MAX);
    return __real_IHS_CondWait(c, m);
}
bool __wrap_IHS_CondWaitTimeout(IHS_Cond *c, IHS_Mutex *m, uint32_t ms) {
    if (video && c == video->base.windowCond)
        recordWait(ms);
    return __real_IHS_CondWaitTimeout(c, m, ms);
}
void __real_IHS_SessionChannelDataReceiveFrame(IHS_SessionChannelData *, IHS_SessionFrame *);
void __wrap_IHS_SessionChannelDataReceiveFrame(IHS_SessionChannelData *ch, IHS_SessionFrame *f) {
    uint16_t id;
    assert(f->body.size >= 13 && *IHS_BufferPointer(&f->body) == k_EStreamDataPacket);
    IHS_ReadUInt16LE(IHS_BufferPointerAt(&f->body, 1), &id);
    assert(consumedCount < IHS_VIDEO_QUEUE_OBSERVATIONS);
    consumed[consumedCount][0] = action;
    consumed[consumedCount++][1] = id;
    __real_IHS_SessionChannelDataReceiveFrame(ch, f);
}
uint32_t __real_IHS_SessionChannelDataThink(IHS_SessionChannel *);
uint32_t __wrap_IHS_SessionChannelDataThink(IHS_SessionChannel *ch) {
    uint32_t timeout = __real_IHS_SessionChannelDataThink(ch);
    if (wakeMode && consumedCount == 1) {
        /* Pause after the real SDK/assembly call, before the FIFO recheck. */
        atomic_store(&inThink, true);
        for (unsigned n = 0; n < 5000 && !atomic_load(&releaseThink); ++n)
            usleep(1000);
        assert(atomic_load(&releaseThink));
    }
    return timeout;
}
static void enqueue(IHS_SessionChannel *ch, unsigned id, unsigned seq, unsigned flags,
                    const uint8_t *bytes, size_t size) {
    IHS_SessionFrame f = {.header = {.type = IHS_SessionPacketTypeReliable,
                                     .channelId = ch->id,
                                     .sendTimestamp = 1000000,
                                     .receiveTimestamp = 1000000,
                                     .receiveBegin = 1000000}};
    IHS_BufferInit(&f.body, 64, 65536);
    IHS_BufferAppendUInt8(&f.body, k_EStreamDataPacket);
    IHS_BufferAppendUInt16LE(&f.body, id);
    IHS_BufferAppendUInt32LE(&f.body, 1000000);
    IHS_BufferAppendUInt16LE(&f.body, 0);
    IHS_BufferAppendUInt32LE(&f.body, 0);
    IHS_BufferAppendUInt16LE(&f.body, seq);
    IHS_BufferAppendUInt8(&f.body, flags);
    IHS_BufferAppendUInt16LE(&f.body, 0);
    IHS_BufferAppendUInt16LE(&f.body, 0);
    IHS_BufferAppendMem(&f.body, bytes, size);
    assert(IHS_SessionDeliverReliableFrame(ch->session, &f) == IHS_ReliableData);
    IHS_SessionFrameClear(&f, true);
}
static void reportQueue(IHS_Session *session) {
#ifndef IHS_VIDEO_PRESSURE_OBSERVER
    (void)session;
#endif
    printf("{\"strict\":%u,\"soft\":%u,\"request\":%u,\"body\":\"", video->states.waitingKeyFrame,
           video->states.syncingBacklog, atomic_load(&video->resetRequested));
    for (size_t n = 0; n < video->frame.buffer.size; ++n)
        printf("%02x", IHS_BufferPointer(&video->frame.buffer)[n]);
    printf("\",\"flow\":[");
    for (unsigned n = 0; n < flowCount; ++n)
        printf("%s%s", n ? "," : "", flow[n]);
    printf("],\"player\":[");
    for (unsigned n = 0; n < heldCount; ++n)
        printf("%s%u", n ? "," : "", pictures[n]);
    printf("],\"waits\":[");
    for (unsigned n = 0; n < waitCount; ++n)
        printf("%s[%u,%u]", n ? "," : "", waits[n][0], waits[n][1]);
    printf("],\"dequeued\":[");
    for (unsigned n = 0; n < consumedCount; ++n)
        printf("%s[%u,%u]", n ? "," : "", consumed[n][0], consumed[n][1]);
#ifdef IHS_VIDEO_PRESSURE_OBSERVER
    printf("],\"closed\":%s,\"tracker_capacity\":%zu}\n",
           session->base.interrupted ? "true" : "false",
           IHS_FrameTrackerStorageCapacity(session->frameTracker));
#else
    puts("]}");
#endif
}
int main(int argc, char **argv) {
    assert(argc == 2 || (argc == 3 && !strcmp(argv[2], "--wake")));
    wakeMode = argc == 3;
    initialBackend = ready = true;
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
    CStartVideoDataMsg c = CSTART_VIDEO_DATA_MSG__INIT;
    c.channel = 3;
    c.codec = k_EStreamVideoCodecH264;
    c.width = 1280;
    c.height = 720;
    IHS_SessionChannel *ch = IHS_SessionChannelDataVideoCreate(s, &c);
    assert(ch);
    IHS_SessionChannelAdd(s, ch);
    video = (void *)ch;
    assert(!video->base.worker && !video->base.discardFrames);
    FILE *clip = fopen(argv[1], "rb");
    uint8_t primer[2204];
    assert(clip && fread(primer, 1, sizeof(primer), clip) == sizeof(primer));
    assert(!fclose(clip));
    feed(ch, 0, 0, 24, primer, sizeof(primer));
    assert(heldCount == 1);
    recording = true;
    holdStartup = false;
    if (wakeMode) {
        enqueue(ch, 1, 1, 8, primer, sizeof(primer));
        IHS_SessionChannelDataRestartWorker(&video->base);
        for (unsigned n = 0; n < 5000 && !atomic_load(&inThink); ++n)
            usleep(1000);
        assert(atomic_load(&inThink));
        enqueue(ch, 2, 2, 8, primer, sizeof(primer));
        atomic_store(&releaseThink, true);
        for (unsigned n = 0; n < 5000 && !atomic_load(&idle) && !s->base.interrupted; ++n)
            usleep(1000);
        assert(atomic_load(&idle) || s->base.interrupted);
        IHS_SessionChannelDataJoinAndPurge(&video->base, true);
        assert(consumedCount == 2 && heldCount == 3 && pictures[1] == 1 && pictures[2] == 2);
        puts("Decoder input during OnThink: no lost wake, worker joined");
    }
    char line[16384], raw[15000];
    while (!wakeMode && fgets(line, sizeof(line), stdin)) {
        if (line[0] == 'F') {
            unsigned fid, seq, flags;
            assert(sscanf(line, "F %u %u %u %14999s", &fid, &seq, &flags, raw) == 4);
            size_t len = strlen(raw) / 2;
            uint8_t bytes[7500];
            assert(strlen(raw) == len * 2);
            for (size_t n = 0; n < len; ++n) {
                unsigned byte;
                assert(sscanf(raw + n * 2, "%2x", &byte) == 1);
                bytes[n] = byte;
            }
            enqueue(ch, fid, seq, flags, bytes, len);
        } else {
            assert(line[0] == 'B' && video->base.fecQueuedCount);
            atomic_store(&idle, false);
            IHS_SessionChannelDataRestartWorker(&video->base);
            assert(video->base.worker);
            for (unsigned n = 0; n < 5000 && !atomic_load(&idle) && !s->base.interrupted; ++n)
                usleep(1000);
            assert(atomic_load(&idle) || s->base.interrupted);
            IHS_SessionChannelDataJoinAndPurge(&video->base, true);
            assert(!video->base.worker && !video->base.fecQueuedCount);
            reportQueue(s);
            if (s->base.interrupted) break;
            ++action;
            /* Resume input admission at the same explicit Source loop boundary. */
            atomic_store(&video->base.interrupted, false);
        }
    }
    recording = false;
    IHS_SessionDestroy(s);
    video = NULL;
    IHS_Quit();
    assert(!heldCount);
    for (unsigned n = 0; n < flowCount; ++n)
        free(flow[n]);
    return 0;
}
