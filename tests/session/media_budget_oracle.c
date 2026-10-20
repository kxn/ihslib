/* Actual RX assembly/dispatch/FIFO/consumer bridge. Thread::Create for the
 * video endpoint is the explicit failed-start adapter; no decoder/thread proof. */
#include "crypto.h"
#include "ihs_buffer.h"
#include "ihslib.h"
#include "session/channels/ch_data.h"
#include "session/reliable.h"
#include "session/session_pri.h"
#include "test_session.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static size_t assembled_size, assembled_capacity, consumed_size, retained_capacity;
static char assembled_sha[65], consumed_sha[65];
static void digest(const IHS_Buffer *body, char output[65]) {
    uint8_t hash[32];
    assert(!IHS_CryptoSHA256(IHS_BufferPointer(body), body->size, hash));
    for (unsigned i = 0; i < 32; ++i)
        sprintf(output + i * 2, "%02x", hash[i]);
}
static unsigned callbacks, consumed, suppressed_starts;
IHS_Thread *__real_IHS_ThreadCreate(IHS_ThreadFunction *, const char *, void *);
IHS_Thread *__wrap_IHS_ThreadCreate(IHS_ThreadFunction *fn, const char *name, void *ctx) {
    if (!strcmp(name, "IHSVideo")) {
        ++suppressed_starts;
        return NULL;
    }
    return __real_IHS_ThreadCreate(fn, name, ctx);
}
bool __wrap_IHS_BaseSend(IHS_Base *base, IHS_SocketAddress address, const IHS_Buffer *wire) {
    (void)base;
    (void)address;
    (void)wire;
    return true;
}
void __real_IHS_SessionChannelDataReceiveReliableFrame(IHS_SessionChannel *, IHS_SessionFrame *);
void __wrap_IHS_SessionChannelDataReceiveReliableFrame(IHS_SessionChannel *ch,
                                                       IHS_SessionFrame *f) {
    ++callbacks;
    assembled_size = f->body.size;
    assembled_capacity = f->body.capacity;
    digest(&f->body, assembled_sha);
    assert(f->mediaPrepared);
    __real_IHS_SessionChannelDataReceiveReliableFrame(ch, f);
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
static void data(IHS_SessionChannel *ch, const IHS_SessionDataFrameHeader *header,
                 IHS_Buffer *body) {
    (void)ch;
    assert(header && header->id == consumed);
    ++consumed;
    consumed_size = body->size;
    digest(body, consumed_sha);
}
static const IHS_SessionChannelDataClass cls = {
    .base = {.instanceSize = sizeof(IHS_SessionChannelData),
             .init = init,
             .deinit = IHS_SessionChannelDataDeinit,
             .stopped = IHS_SessionChannelDataStopped,
             .received = IHS_SessionChannelDataReceived},
    .startOnCaller = true,
    .start = start,
    .stop = stop,
    .dataFrame = data};
static bool poll(IHS_SessionChannelData *ch) {
    IHS_SessionFrame frame = {0};
    uint16_t missing = 99;
    IHS_MutexLock(ch->windowLock);
    bool ready = IHS_SessionChannelDataPollLocked(ch, &frame, &missing);
    IHS_MutexUnlock(ch->windowLock);
    assert(!missing);
    if (ready) {
        retained_capacity = frame.body.capacity;
        IHS_SessionChannelDataReceiveFrame(ch, &frame);
    }
    IHS_BufferClear(&frame.body, true);
    return ready;
}
int main(int argc, char **argv) {
    assert(argc >= 2 && argc <= 4);
    size_t length = strtoul(argv[1], NULL, 10),
           chunk = argc >= 3 ? strtoul(argv[2], NULL, 10) : 1200;
    assert(chunk >= 20 && chunk <= 2035);
    size_t count = (length + chunk - 1) / chunk;
    bool reliable = argc == 4 && !strcmp(argv[3], "reliable");
    assert((argc < 4 || reliable) && length >= 20 && count < 2048);
    IHS_Init();
    IHS_Session *s = IHS_TestSessionCreate();
    assert(s);
    IHS_TimerTaskStopImmediate(s->retransmission.timer);
    s->state.connectionState = IHS_SessionConnectionStateConnected;
    IHS_SessionChannelData *ch =
        (void *)IHS_SessionChannelDataCreate(&cls, s, IHS_SessionChannelTypeDataVideo, 9, NULL);
    assert(ch);
    IHS_SessionChannelAdd(s, &ch->base);
    assert(suppressed_starts == 1 && !ch->worker && ch->callerStarted && !ch->discardFrames);
    for (size_t i = 0, off = 0; i < count; ++i) {
        size_t size = length - off < chunk ? length - off : chunk;
        unsigned char *bytes = calloc(size, 1);
        assert(bytes);
        bytes[0] = 1;
        IHS_SessionPacket packet = {
            .header = {.channelId = 9,
                       .type = i ? (reliable ? IHS_SessionPacketTypeReliableFrag
                                             : IHS_SessionPacketTypeUnreliableFrag)
                                 : (reliable ? IHS_SessionPacketTypeReliable
                                             : IHS_SessionPacketTypeUnreliable),
                       .packetId = (uint16_t)i,
                       .fragmentId = (int16_t)(i ? i - 1 : count - 1),
                       .sendTimestamp = 1000000,
                       .receiveTimestamp = 1000000},
            .body = IHS_BUFFER_WRAP(bytes, size)};
        if (reliable)
            IHS_SessionReliableReceived(s, &packet);
        else
            IHS_SessionUnreliableReceived(s, &packet);
        IHS_SessionPacketClear(&packet, true);
        off += size;
    }
    if (reliable)
        IHS_SessionReliableUpdateChannelAt(s, 9, 1000000);
    else
        IHS_SessionUnreliableUpdateAt(s, 9, 1000000);
    assert(callbacks == 1 && assembled_size == length);
    size_t capacity = assembled_capacity, queued = ch->fecQueuedBytes, limit = ch->fecQueueLimit;
    unsigned queued_count = ch->fecQueuedCount;
    bool large_ready = poll(ch);
    char raw_sha[65], payload_sha[65];
    strcpy(raw_sha, assembled_sha);
    strcpy(payload_sha, consumed_sha);
    assert(large_ready && consumed_size == length - 13);
    size_t kept_capacity = retained_capacity;
    uint8_t small[20] = {1, 1};
    IHS_SessionPacket next = {.header = {.channelId = 9,
                                         .type = reliable ? IHS_SessionPacketTypeReliable
                                                          : IHS_SessionPacketTypeUnreliable,
                                         .packetId = (uint16_t)count,
                                         .sendTimestamp = 1000001,
                                         .receiveTimestamp = 1000001},
                              .body = IHS_BUFFER_INIT(20, 20)};
    IHS_BufferAppendMem(&next.body, small, sizeof(small));
    if (reliable)
        IHS_SessionReliableReceived(s, &next);
    else
        IHS_SessionUnreliableReceived(s, &next);
    IHS_SessionPacketClear(&next, true);
    if (reliable)
        IHS_SessionReliableUpdateChannelAt(s, 9, 1000001);
    else
        IHS_SessionUnreliableUpdateAt(s, 9, 1000001);
    assert(poll(ch) && consumed == 2 && consumed_size == 7 && !ch->fecQueuedCount &&
           !ch->fecQueuedBytes);
    printf("{\"body\":%zu,\"fragments\":%zu,\"assembly_capacity\":%zu,\"retained_capacity\":%zu,"
           "\"fifo_bytes\":%zu,\"fifo_"
           "count\":%u,\"fifo_limit\":%zu,\"large_consumed\":%s,\"following_consumed\":true,"
           "\"window_remaining\":%u,\"failed_start_adapter\":%u,\"raw_sha256\":\"%s\",\"consumer_"
           "sha256\":\"%s\"}\n",
           length, count, capacity, kept_capacity, queued, queued_count, limit,
           large_ready ? "true" : "false",
           IHS_SessionPacketsWindowSize(reliable ? s->reliable[9].framePacketWindow
                                                 : ch->receive->window),
           suppressed_starts, raw_sha, payload_sha);
    IHS_SessionChannelRemove(s, 9);
    IHS_SessionDestroy(s);
    IHS_Quit();
    return 0;
}
