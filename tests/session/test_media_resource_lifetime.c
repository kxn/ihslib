/* Production FIFO compaction and resource-failure close. Allocation failures,
 * final Socket and failed video Thread::Create are explicit local adapters;
 * there is no codec, actual decoder worker, Steam host or Switch claim. */
#include "ihs_buffer.h"
#include "ihs_buffer_ext.h"
#include "session/channels/ch_data.h"
#include "session/reliable.h"
#include "session/session_pri.h"
#include "test_session.h"
#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static bool fail_realloc, fail_node, expect_close;
static size_t node_bytes;
static void *freed_pointer;
static unsigned freed_count, close_count, feedback_count;
static IHS_Session *watched_session;
static IHS_SessionChannelData *watched_channel;
void *__real_realloc(void *, size_t);
void *__wrap_realloc(void *p, size_t n) {
    if (fail_realloc) {
        fail_realloc = false;
        return NULL;
    }
    return __real_realloc(p, n);
}
void *__real_malloc(size_t);
void *__wrap_malloc(size_t n) {
    if (fail_node && n == node_bytes) {
        fail_node = false;
        return NULL;
    }
    return __real_malloc(n);
}
void __real_free(void *);
void __wrap_free(void *p) {
    if (p && p == freed_pointer)
        ++freed_count;
    __real_free(p);
}
IHS_Thread *__real_IHS_ThreadCreate(IHS_ThreadFunction *, const char *, void *);
IHS_Thread *__wrap_IHS_ThreadCreate(IHS_ThreadFunction *fn, const char *name, void *ctx) {
    if (!strcmp(name, "IHSVideo"))
        return NULL;
    return __real_IHS_ThreadCreate(fn, name, ctx);
}
bool __wrap_IHS_BaseSend(IHS_Base *base, IHS_SocketAddress address, const IHS_Buffer *data) {
    (void)base;
    (void)address;
    const uint8_t *wire = IHS_BufferPointer(data);
    unsigned type = wire[0] & 127;
    if (type == 7 || type == 8 || type == 10 ||
        (type == 3 && data->size > 13 && wire[13] == k_EStreamDataLost))
        ++feedback_count;
    return true;
}
static void *probe_locks(void *unused) {
    (void)unused;
    IHS_Session *s = watched_session;
    IHS_BaseLock(&s->base);
    IHS_BaseUnlock(&s->base);
    IHS_MutexLock(s->reliableLock);
    IHS_MutexUnlock(s->reliableLock);
    IHS_MutexLock(watched_channel->receive->lock);
    IHS_MutexUnlock(watched_channel->receive->lock);
    IHS_MutexLock(watched_channel->windowLock);
    IHS_MutexUnlock(watched_channel->windowLock);
    return NULL;
}
void __real_IHS_SessionDisconnect(IHS_Session *);
void __wrap_IHS_SessionDisconnect(IHS_Session *s) {
    if (expect_close) {
        assert(s == watched_session);
        expect_close = false;
        ++close_count;
        pthread_t probe;
        assert(!pthread_create(&probe, NULL, probe_locks, NULL));
        assert(!pthread_join(probe, NULL));
    }
    __real_IHS_SessionDisconnect(s);
}
static void init(IHS_SessionChannel *ch, const void *unused) {
    (void)unused;
    assert(IHS_SessionChannelDataInit(ch, 2048));
}
static bool start(IHS_SessionChannel *ch) {
    (void)ch;
    return true;
}
static void stop(IHS_SessionChannel *ch) {
    (void)ch;
}
static const IHS_SessionChannelDataClass cls = {
    .base = {.instanceSize = sizeof(IHS_SessionChannelData),
             .init = init,
             .deinit = IHS_SessionChannelDataDeinit,
             .stopped = IHS_SessionChannelDataStopped,
             .received = IHS_SessionChannelDataReceived},
    .startOnCaller = true,
    .start = start,
    .stop = stop};
static IHS_Session *setup(IHS_SessionChannelData **out) {
    IHS_Session *s = IHS_TestSessionCreate();
    assert(s);
    IHS_TimerTaskStopImmediate(s->retransmission.timer);
    s->state.connectionState = IHS_SessionConnectionStateConnected;
    *out = (void *)IHS_SessionChannelDataCreate(&cls, s, IHS_SessionChannelTypeDataVideo, 9, NULL);
    assert(*out);
    IHS_SessionChannelAdd(s, &(*out)->base);
    assert(!(*out)->worker && (*out)->callerStarted);
    return s;
}
static IHS_SessionFrame frame(size_t capacity, size_t offset, size_t size, size_t suffix) {
    assert(offset + size + suffix <= capacity && size >= 13);
    IHS_SessionFrame f = {.header = {.channelId = 9, .type = IHS_SessionPacketTypeUnreliable},
                          .body = IHS_BUFFER_INIT(1, 0)};
    IHS_BufferEnsureCapacityExact(&f.body, capacity);
    memset(f.body.data, 0xa5, capacity);
    f.body.offset = offset;
    f.body.size = size;
    f.body.suffix = suffix;
    f.body.data[offset] = k_EStreamDataPacket;
    memset(f.body.data + offset + 1, 0, 12);
    return f;
}
static bool poll(IHS_SessionChannelData *ch, IHS_SessionFrame *f) {
    uint16_t missing = 99;
    IHS_MutexLock(ch->windowLock);
    bool ready = IHS_SessionChannelDataPollLocked(ch, f, &missing);
    IHS_MutexUnlock(ch->windowLock);
    assert(!missing);
    return ready;
}
static void queue_cases(void) {
    IHS_SessionChannelData *ch;
    IHS_Session *s = setup(&ch);
    IHS_SessionFrame f = frame(20, 0, 20, 0), out = {0};
    IHS_SessionChannelDataReceiveReliableFrame(&ch->base, &f);
    assert(ch->fecQueuedCount == 1 && ch->fecQueuedBytes > 20);
    node_bytes = ch->fecQueuedBytes - 20;
    assert(poll(ch, &out));
    IHS_BufferClear(&out.body, true);
    ch->fecQueueLimit = node_bytes + 64;
    f = frame(128, 7, 20, 5);
    uint8_t expected[32];
    memcpy(expected, f.body.data, 32);
    IHS_SessionChannelDataReceiveReliableFrame(&ch->base, &f);
    assert(!f.body.data && ch->fecQueuedBytes == node_bytes + 32 && ch->fecQueuedCount == 1);
    assert(poll(ch, &out));
    assert(out.body.capacity == 32 && out.body.offset == 7 && out.body.size == 20 &&
           out.body.suffix == 5);
    assert(!memcmp(out.body.data, expected, 32));
    IHS_BufferClear(&out.body, true);
    /* Failed compaction must release the new frame exactly once and retain an
     * already queued frame; it must not start retiring the FIFO first. */
    f = frame(20, 0, 20, 0);
    IHS_SessionChannelDataReceiveReliableFrame(&ch->base, &f);
    f = frame(128, 7, 20, 5);
    freed_pointer = f.body.data;
    freed_count = 0;
    fail_realloc = true;
    IHS_SessionChannelDataReceiveReliableFrame(&ch->base, &f);
    assert(!fail_realloc && freed_count == 1 && !f.body.data && ch->fecQueuedCount == 1 &&
           ch->fecQueuedBytes == node_bytes + 20);
    freed_pointer = NULL;
    assert(poll(ch, &out));
    IHS_BufferClear(&out.body, true);
    /* Genuine used-byte excess is still rejected without compacting. */
    f = frame(128, 0, 65, 0);
    freed_pointer = f.body.data;
    freed_count = 0;
    IHS_SessionChannelDataReceiveReliableFrame(&ch->base, &f);
    assert(!f.body.data && freed_count == 1 && !ch->fecHead);
    freed_pointer = NULL;
    f = frame(20, 0, 20, 0);
    freed_pointer = f.body.data;
    freed_count = 0;
    fail_node = true;
    IHS_SessionChannelDataReceiveReliableFrame(&ch->base, &f);
    assert(!fail_node && !f.body.data && freed_count == 1 && !ch->fecHead);
    freed_pointer = NULL;
    /* Impossible local descriptor arithmetic is rejected before pointer access. */
    f = frame(64, 0, 20, 0);
    f.body.offset = SIZE_MAX;
    freed_pointer = f.body.data;
    freed_count = 0;
    IHS_SessionChannelDataReceiveReliableFrame(&ch->base, &f);
    assert(!f.body.data && freed_count == 1 && !ch->fecHead);
    freed_pointer = NULL;
    IHS_SessionChannelRemove(s, 9);
    IHS_SessionDestroy(s);
}
static void padded_fec(void) {
    IHS_SessionChannelData *ch;
    IHS_Session *s = setup(&ch);
    /* Post-admission synthetic shape: k1/m254/S17000, not negotiated UDP MTU.
     * Systematic data completes without RS. Encoded storage exceeds the FIFO. */
    IHS_SessionPacket p = {.header = {.channelId = 9, .type = IHS_SessionPacketTypeUnreliableFEC},
                           .body = IHS_BUFFER_INIT(17008, 17008)};
    IHS_BufferAppendUInt32LE(&p.body, 20);
    IHS_BufferAppendUInt16LE(&p.body, 17000);
    IHS_BufferAppendUInt16LE(&p.body, 254);
    IHS_BufferFillMem(&p.body, p.body.size, 0, 17000);
    p.body.data[8] = k_EStreamDataPacket;
    IHS_SessionUnreliableReceived(s, &p);
    IHS_SessionPacketClear(&p, true);
    assert(ch->fecQueuedCount == 1 && ch->fecQueuedBytes == node_bytes + 20);
    IHS_SessionFrame out = {0};
    assert(poll(ch, &out));
    assert(out.header.type == IHS_SessionPacketTypeUnreliableFEC && out.body.size == 20 &&
           out.body.capacity == 20);
    assert(out.body.data[0] == k_EStreamDataPacket);
    for (unsigned i = 1; i < 20; ++i)
        assert(!out.body.data[i]);
    IHS_BufferClear(&out.body, true);
    IHS_SessionChannelRemove(s, 9);
    IHS_SessionDestroy(s);
}
static void allocation_close(bool reliable) {
    IHS_SessionChannelData *ch;
    IHS_Session *s = setup(&ch);
    IHS_SessionPacket p = {.header = {.channelId = 9,
                                      .type = reliable ? IHS_SessionPacketTypeReliable
                                                       : IHS_SessionPacketTypeUnreliable},
                           .body = IHS_BUFFER_INIT(20, 20)};
    IHS_BufferFillMem(&p.body, 0, 0, 20);
    p.body.data[0] = k_EStreamDataPacket;
    if (reliable)
        IHS_SessionReliableReceived(s, &p);
    else
        IHS_SessionUnreliableReceived(s, &p);
    IHS_SessionPacketClear(&p, true);
    watched_session = s;
    watched_channel = ch;
    expect_close = true;
    fail_realloc = true;
    unsigned before = close_count;
    feedback_count = 0;
    if (reliable)
        IHS_SessionReliableUpdateChannelAt(s, 9, 1000000);
    else
        IHS_SessionUnreliableUpdateAt(s, 9, 1000000);
    assert(!fail_realloc && !expect_close && close_count == before + 1 && !feedback_count);
    assert(atomic_load(&s->transport.closeRequested) && s->base.interrupted);
    IHS_SessionDestroy(s);
    watched_session = NULL;
    watched_channel = NULL;
}
int main(void) {
    alarm(10);
    IHS_Init();
    queue_cases();
    padded_fec();
    allocation_close(false);
    allocation_close(true);
    IHS_Quit();
    alarm(0);
    return 0;
}
