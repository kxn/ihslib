/* Real worker stays alive between SDK gates. The producer runs while its
 * consumer is blocked in the declared SDK terminal; no class/state is replaced. */
#define IHS_VIDEO_QUEUE_EXTERNAL_MAIN 1
#define IHS_VIDEO_REAL_WAITS 1
#define IHS_VIDEO_BUDGET_OBSERVER 1
#include "video_pressure_oracle.c"
#include <errno.h>

static pthread_mutex_t gateMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gateCond = PTHREAD_COND_INITIALIZER;
static atomic_uint entered, released;
static bool releaseAll;
static uint16_t blockedId;
static IHS_Buffer *blockedBuffer;
static unsigned gateCount;
static bool budgetMode;
static atomic_uint budgetCalls, budgetElapsed;
static atomic_bool injectEintr;
static atomic_uint interruptedReturns;
int __real_pthread_cond_wait(pthread_cond_t *, pthread_mutex_t *);
int __wrap_pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex) {
    int result = __real_pthread_cond_wait(cond, mutex);
    /* POSIX IHS_Cond starts with pthread_cond_t. Return EINTR from the real
     * awakened decoder wait while keeping its mutex acquisition intact. */
    if (video && (void *)cond == (void *)video->base.windowCond && atomic_exchange(&injectEintr, false)) {
        atomic_fetch_add(&interruptedReturns, 1);
        return EINTR;
    }
    return result;
}
/* Same declared 15-second OS budget clock as the Source terminal. Real
 * pthread waits still execute; wall-clock SDK/timeout precision is not claimed. */
uint64_t __wrap_IHS_TimerNow(void) { return 15000 + atomic_load(&budgetElapsed); }
bool IHS_TestDecoderTimedWait(uint32_t ms, bool *handled) {
    *handled = budgetMode;
    if (!budgetMode) return false;
    assert(ms == 1);
    atomic_fetch_add(&budgetElapsed, 1);
    return atomic_fetch_add(&budgetCalls, 1) == 0; /* First spurious, then timeout. */
}
static void interruptSlow(IHS_Session *s, const IHS_VideoEpochInfo *epoch, bool restarting, void *ctx) {
    (void)s; (void)epoch; (void)restarting; (void)ctx;
    pthread_mutex_lock(&gateMutex);
    releaseAll = true;
    pthread_cond_broadcast(&gateCond);
    pthread_mutex_unlock(&gateMutex);
}
static void thinkSlow(IHS_Session *s, const IHS_VideoEpochInfo *epoch, void *ctx) {
    (void)s; (void)epoch; (void)ctx;
}

static IHS_StreamVideoSubmitResult slowSubmit(IHS_Session *s, const IHS_VideoEpochInfo *e,
    uint16_t id, IHS_FrameTicket *ticket, IHS_Buffer *b, IHS_StreamVideoFrameFlag flags,
    bool *taken, void *ctx) {
    if (recording) {
        pthread_mutex_lock(&gateMutex);
        blockedId = id;
        blockedBuffer = b;
        unsigned current = atomic_fetch_add(&entered, 1) + 1;
        pthread_cond_broadcast(&gateCond);
        while (!releaseAll && atomic_load(&released) < current)
            pthread_cond_wait(&gateCond, &gateMutex);
        pthread_mutex_unlock(&gateMutex);
    }
    return submit(s, e, id, ticket, b, flags, taken, ctx);
}
static void waitBoundary(void) {
    for (unsigned n = 0; n < 15000; ++n) {
        if (atomic_load(&entered) > atomic_load(&released) || atomic_load(&idle)) return;
        usleep(1000);
    }
    assert(false);
}
static void resumeOne(void) {
    atomic_store(&idle, false);
    pthread_mutex_lock(&gateMutex);
    atomic_store(&released, atomic_load(&entered));
    pthread_cond_broadcast(&gateCond);
    pthread_mutex_unlock(&gateMutex);
    waitBoundary();
}
static void snapshot(IHS_Session *session, bool gate) {
    printf("{\"id\":%u,\"thinks\":%u,\"payload\":\"", gate ? blockedId : 0, atomic_load(&actualThinks));
    if (gate)
        for (size_t n = 0; n < blockedBuffer->size; ++n)
            printf("%02x", IHS_BufferPointer(blockedBuffer)[n]);
    printf("\",\"queue\":[");
    IHS_MutexLock(video->base.windowLock);
    /* Read-only copy of the private node representation declared in ch_data.c.
     * memcpy avoids aliasing a different struct tag. No ownership is transferred. */
    struct NodeView { IHS_SessionFrame frame; void *next; };
    void *node = video->base.fecHead;
    unsigned count = 0;
    while (node) {
        struct NodeView view;
        memcpy(&view, node, sizeof(view));
        uint16_t id;
        assert(view.frame.body.size >= 3);
        IHS_ReadUInt16LE(IHS_BufferPointerAt(&view.frame.body, 1), &id);
        printf("%s%u", count ? "," : "", id);
        node = view.next;
        assert(++count <= 4096);
    }
    IHS_MutexUnlock(video->base.windowLock);
    printf("],\"tasks\":[");
    for (unsigned n = 0; n < heldCount; ++n) {
        IHS_FrameIdentity identity = IHS_FrameTicketIdentity(held[n]);
        uint32_t stamp;
        assert(IHS_FrameTicketVideoTimestamp(held[n], &stamp));
        printf("%s[%llu,%u,%u]", n ? "," : "", (unsigned long long)identity.receiveSerial,
               identity.frameId, stamp);
    }
    printf("],\"state\":");
    reportQueue(session);
    putchar('}');
}
static void snapshotGate(IHS_Session *s) {
    if (gateCount++) putchar(',');
    snapshot(s, true);
}
static unsigned hexDigit(char value) {
    if (value >= '0' && value <= '9') return (unsigned)(value - '0');
    if (value >= 'a' && value <= 'f') return (unsigned)(value - 'a' + 10);
    if (value >= 'A' && value <= 'F') return (unsigned)(value - 'A' + 10);
    assert(false); return 0;
}
int main(int argc, char **argv) {
    assert(argc == 2 || (argc == 3 && (!strcmp(argv[2], "--stop") || !strcmp(argv[2], "--deadline"))));
    budgetMode = argc == 3 && !strcmp(argv[2], "--deadline");
    initialBackend = ready = true;
    IHS_Init();
    IHS_Session *s = IHS_TestSessionCreate();
    assert(s);
    IHS_TimerTaskStopImmediate(s->retransmission.timer);
    s->state.connectionState = IHS_SessionConnectionStateConnected;
    assert(IHS_SessionSetVideoTrackingIdentity(s, 90));
    IHS_StreamVideoCallbacks cb = {.prepare=prepare, .startTracked=start, .submitTracked=slowSubmit,
                                   .stopTracked=stop, .flushTracked=flush,
                                   .interruptTracked=interruptSlow, .thinkTracked=thinkSlow};
    IHS_SessionSetVideoCallbacks(s, &cb, NULL);
    CStartVideoDataMsg config = CSTART_VIDEO_DATA_MSG__INIT;
    config.channel=3; config.codec=k_EStreamVideoCodecH264; config.width=1280; config.height=720;
    IHS_SessionChannel *channel = IHS_SessionChannelDataVideoCreate(s, &config);
    assert(channel); IHS_SessionChannelAdd(s, channel); video=(void *)channel;
    FILE *clip=fopen(argv[1], "rb"); uint8_t primer[2204];
    assert(clip && fread(primer, 1, sizeof(primer), clip)==sizeof(primer) && !fclose(clip));
    feed(channel, 0, 0, 24, primer, sizeof(primer));
    assert(heldCount==1 && !video->base.worker);
    recording=true; holdStartup=false;
    printf("{\"gates\":[");
    char *line=NULL; size_t lineCapacity=0; bool started=false;
    while (getline(&line, &lineCapacity, stdin)>=0) {
        if (line[0]=='F' || line[0]=='A') {
            unsigned id,seq,flags,stamp; int offset=0;
            assert(sscanf(line, "%*c %u %u %u %u %n", &id,&seq,&flags,&stamp,&offset)==4 && offset>0);
            const char *raw=line+offset;
            size_t hexSize=strcspn(raw,"\r\n"),size=hexSize/2;
            assert(hexSize==size*2 && size && size<=2*1024*1024);
            uint8_t *bytes=malloc(size); assert(bytes);
            for (size_t n=0;n<size;++n)
                bytes[n]=(uint8_t)((hexDigit(raw[n*2])<<4)|hexDigit(raw[n*2+1]));
            if (line[0]=='A') {
                IHS_MutexLock(video->base.windowLock);
                atomic_store(&idle,false);
                IHS_MutexUnlock(video->base.windowLock);
            }
            enqueueStamped(channel,id,seq,flags,stamp,bytes,size);
            free(bytes);
            if (line[0]=='A') waitBoundary();
        } else if (line[0]=='S') {
            assert(!started); started=true; atomic_store(&idle,false);
            IHS_SessionChannelDataRestartWorker(&video->base); waitBoundary();
        } else if (line[0]=='U' || line[0]=='E') {
            assert(started && atomic_load(&idle));
            IHS_MutexLock(video->base.windowLock);
            atomic_store(&idle, false);
            atomic_store(&injectEintr, line[0]=='E');
            IHS_CondSignal(video->base.windowCond); /* No input/event: deliberate spurious wake. */
            IHS_MutexUnlock(video->base.windowLock);
            waitBoundary();
        } else if (line[0]=='T') {
            assert(argc == 3 && started && !atomic_load(&idle));
            IHS_SessionChannelDataJoinAndPurge(&video->base, false);
            assert(!video->base.worker && !video->base.fecQueuedCount);
            recording=false;
            IHS_SessionDestroy(s); video=NULL; IHS_Quit();
            printf("],\"stopped\":true,\"joined\":true,\"released\":%s}\n",
                   !heldCount && !haveBackend ? "true" : "false");
            for (unsigned n=0;n<flowCount;++n) free(flow[n]);
            free(line);
            assert(!pthread_cond_destroy(&gateCond) && !pthread_mutex_destroy(&gateMutex));
            return 0;
        } else {
            assert(line[0]=='R' && started);
            if (atomic_load(&idle)) break; /* A semantic regression may stop early. */
            snapshotGate(s); resumeOne();
        }
    }
    free(line);
    assert(started);
    /* Drain any extra submissions from a regression with no further producer
     * input. Normal exits, rather than fixture hangs, must expose wrong output. */
    while (!atomic_load(&idle)) { snapshotGate(s); resumeOne(); assert(gateCount<4096); }
    printf("],\"end\":"); snapshot(s, false);
    IHS_SessionChannelDataJoinAndPurge(&video->base,true);
    printf(",\"joined\":%s,\"budget_calls\":%u,\"budget_elapsed\":%u,\"interrupted_returns\":%u}\n",
           !video->base.worker ? "true" : "false", atomic_load(&budgetCalls), atomic_load(&budgetElapsed),
           atomic_load(&interruptedReturns));
    recording=false; IHS_SessionDestroy(s); video=NULL; IHS_Quit();
    assert(!heldCount);
    for (unsigned n=0;n<flowCount;++n) free(flow[n]);
    assert(!pthread_cond_destroy(&gateCond) && !pthread_mutex_destroy(&gateMutex));
    return 0;
}
