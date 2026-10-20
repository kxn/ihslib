/* Real session ownership, complete-frame preparation, stopping and concurrent
 * public snapshots. Allocation failure is a local guard, not source behavior. */
#include "hid/manager.h"
#include "ihs_buffer_ext.h"
#include "ihslib.h"
#include "session/channels/ch_data.h"
#include "session/session_pri.h"
#include "test_session.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
static _Thread_local bool failAllocation;
static IHS_Session *session;
static bool trackingStop, playerStopped, inputStopped, statsStopped;
static atomic_bool done;
void *__real_realloc(void *, size_t);
void *__wrap_realloc(void *p, size_t size) {
    if (failAllocation) {
        failAllocation = false;
        return NULL;
    }
    return __real_realloc(p, size);
}
void __real_IHS_HIDManagerCloseAll(IHS_HIDManager *);
void __wrap_IHS_HIDManagerCloseAll(IHS_HIDManager *m) {
    if (trackingStop) {
        assert(playerStopped && !session->mediaNetworkStats.finalized);
        inputStopped = true;
    }
    __real_IHS_HIDManagerCloseAll(m);
}
bool __real_IHS_MediaNetworkStatsStop(IHS_MediaNetworkStats *);
bool __wrap_IHS_MediaNetworkStatsStop(IHS_MediaNetworkStats *s) {
    if (trackingStop) {
        assert(playerStopped && inputStopped && s == &session->mediaNetworkStats);
        assert(!IHS_SessionChannelForType(session, IHS_SessionChannelTypeDataAudio));
        assert(!IHS_SessionChannelForType(session, IHS_SessionChannelTypeDataVideo));
        assert(session->state.connectionState == IHS_SessionConnectionStateStopping);
        statsStopped = true;
    }
    return __real_IHS_MediaNetworkStatsStop(s);
}
static void stopped(IHS_Session *s, void *unused) {
    (void)unused;
    assert(trackingStop && !s->mediaNetworkStats.finalized);
    assert(!IHS_SessionChannelForType(s, IHS_SessionChannelTypeDataAudio));
    assert(!IHS_SessionChannelForType(s, IHS_SessionChannelTypeDataVideo));
    playerStopped = true;
}
static void prepare(IHS_SessionChannelType type, uint16_t id, bool failCollector) {
    IHS_SessionChannel channel = {.session = session, .type = type};
    IHS_SessionFrame f = {
        .header = {.sendTimestamp = 1000, .receiveTimestamp = 1100, .receiveBegin = 1090}};
    IHS_BufferInit(&f.body, 64, 64);
    IHS_BufferAppendUInt8(&f.body, k_EStreamDataPacket);
    IHS_BufferAppendUInt16LE(&f.body, id);
    IHS_BufferAppendUInt32LE(&f.body, 1000);
    IHS_BufferAppendUInt16LE(&f.body, 0);
    IHS_BufferAppendUInt32LE(&f.body, 0);
    if (failCollector)
        failAllocation = true;
    IHS_SessionChannelDataPrepareFrame(&channel, &f);
    assert(f.mediaPrepared);
    uint32_t total = session->mediaNetworkStats.total;
    IHS_SessionChannelDataPrepareFrame(&channel, &f);
    assert(session->mediaNetworkStats.total == total);
    IHS_BufferClear(&f.body, true);
}
static void *reader(void *unused) {
    (void)unused;
    unsigned reads = 0;
    do {
        IHS_SessionMediaNetworkStats s;
        IHS_SessionGetMediaNetworkStats(session, &s);
        assert(s.valid && s.windowReceived < 600);
        assert(s.windowFrameSpan == s.windowReceived);
        assert(s.frameSpan == s.windows * 600u + s.windowReceived);
    } while (++reads < 512 && !atomic_load(&done));
    return NULL;
}
int main(void) {
    IHS_SessionMediaNetworkStats empty;
    memset(&empty, 0xff, sizeof(empty));
    IHS_SessionGetMediaNetworkStats(NULL, &empty);
    assert(!empty.valid && !empty.frameSpan);
    IHS_SessionGetMediaNetworkStats(NULL, NULL);
    IHS_Init();
    session = IHS_TestSessionCreate();
    assert(session);
    IHS_TimerTaskStopImmediate(session->retransmission.timer);
    IHS_MediaNetworkStats *s = &session->mediaNetworkStats;
    prepare(IHS_SessionChannelTypeDataAudio, 20, false);
    assert(!s->total && !s->count);
    prepare(IHS_SessionChannelTypeDataVideo, 0, false);
    assert(s->total == 1 && s->count == 1);
    /* First allocation failure leaves media preparation successful. */
    prepare(IHS_SessionChannelTypeDataVideo, 599, true);
    assert(!failAllocation && !s->valid && s->total == 600 && !s->count);
    prepare(IHS_SessionChannelTypeDataVideo, 600, false);
    assert(s->total == 601 && !s->valid);
    IHS_MediaNetworkStatsStart(s);
    for (unsigned i = 0; i < 600; ++i)
        assert(IHS_MediaNetworkStatsRecord(s, i, 1000, 1100));
    assert(s->valid && s->windows && s->windowCount == 1);
    size_t capacity = s->capacity;
    while (s->windowCount < capacity)
        assert(IHS_MediaNetworkStatsFold(s));
    IHS_MediaNetworkWindow *allocation = s->windows;
    failAllocation = true;
    assert(!IHS_MediaNetworkStatsFold(s));
    assert(!s->valid && s->windows == allocation && s->windowCount == capacity);
    IHS_MediaNetworkStatsStart(s);
    assert(s->valid && s->windows == allocation && !s->windowCount);
    pthread_t worker;
    assert(!pthread_create(&worker, NULL, reader, NULL));
    for (unsigned i = 0; i < 12000; ++i) {
        IHS_BaseLock(&session->base);
        assert(IHS_MediaNetworkStatsRecord(s, i, 1000, 1100));
        IHS_BaseUnlock(&session->base);
    }
    atomic_store(&done, true);
    assert(!pthread_join(worker, NULL));
    IHS_MediaNetworkStatsStart(s);
    prepare(IHS_SessionChannelTypeDataVideo, 0, false);
    const IHS_SessionChannelClass cls = {.instanceSize = sizeof(IHS_SessionChannel)};
    for (unsigned i = 0; i < 2; ++i) {
        IHS_SessionChannel *ch = IHS_SessionChannelCreate(
            &cls, session, i ? IHS_SessionChannelTypeDataVideo : IHS_SessionChannelTypeDataAudio,
            3 + i, NULL);
        assert(ch);
        IHS_SessionChannelAdd(session, ch);
    }
    const IHS_StreamSessionCallbacks callbacks = {.disconnected = stopped};
    IHS_SessionSetSessionCallbacks(session, &callbacks, NULL);
    session->state.connectionState = IHS_SessionConnectionStateStopping;
    trackingStop = true;
    IHS_SessionClientStopStreaming(session);
    trackingStop = false;
    assert(playerStopped && inputStopped && statsStopped && s->finalized);
    assert(s->average.loss == 0 && s->average.mean > 0);
    assert(session->state.connectionState == IHS_SessionConnectionStateUnconnected);
    IHS_SessionDestroy(session);
    IHS_Quit();
    return 0;
}
