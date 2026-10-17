/* Batch-end feedback, assembly boundaries and timestamp wrap. Real decoder
 * worker can block while the network owner advances windows and sends NACKs. */
#include "endianness.h"
#include "ihs_buffer.h"
#include "ihs_buffer_ext.h"
#include "ihs_queue.h"
#include "session/channels/ch_data.h"
#include "session/session_pri.h"
#include "test_session.h"
#include <assert.h>
#include <pthread.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static uint8_t feedbackWire[128];
static size_t feedbackSize;
bool __wrap_IHS_BaseSend(IHS_Base *base, IHS_SocketAddress address, const IHS_Buffer *wire) {
    (void)base; (void)address;
    if ((*IHS_BufferPointer(wire) & 127) == IHS_SessionPacketTypeUnreliableNack) {
        assert(wire->size <= sizeof(feedbackWire));
        feedbackSize = wire->size;
        memcpy(feedbackWire, IHS_BufferPointer(wire), feedbackSize);
    }
    return true;
}
static void add(IHS_SessionPacketsWindow *w, uint16_t id, uint32_t stamp,
                IHS_SessionPacketType type, int16_t fragment) {
    IHS_SessionPacket p = {.header = {.type = type,
                                      .packetId = id,
                                      .fragmentId = fragment,
                                      .sendTimestamp = stamp,
                                      .receiveTimestamp = 500,
                                      .retransmitCount = 0}};
    IHS_BufferInit(&p.body, 4, 4);
    IHS_BufferAppendUInt8(&p.body, (uint8_t)id);
    assert(IHS_SessionPacketsWindowAdd(w, &p));
    IHS_SessionPacketClear(&p, true);
}
static void window_cases(void) {
    IHS_StreamClock clock;
    IHS_StreamClockInit(&clock);
    IHS_StreamClockFeedback(&clock, 1000, 2000, 1656); // ~10ms RTT
    assert(IHS_StreamClockMediaDelayTicks(&clock) == 6946);
    assert(IHS_StreamClockFeedbackTicks(&clock) == 164);
    IHS_StreamClockDeinit(&clock);
    IHS_SessionPacketsWindow *initial = IHS_SessionPacketsWindowCreateMedia(32);
    IHS_MediaFeedback initialFeedback;
    add(initial, 1, 1000, IHS_SessionPacketTypeUnreliableFrag, 0);
    assert(IHS_SessionPacketsWindowMediaFeedback(initial, &initialFeedback));
    assert(initialFeedback.confirmed == 65535 && initialFeedback.base == 1);
    IHS_SessionFrame joined = {0};
    IHS_BufferInit(&joined.body, 8, 32);
    assert(!IHS_SessionPacketsWindowPoll(initial, &joined));
    add(initial, 0, 1000, IHS_SessionPacketTypeUnreliable, 1);
    assert(IHS_SessionPacketsWindowPoll(initial, &joined) && joined.body.size == 2);
    IHS_BufferClear(&joined.body, true);
    IHS_SessionPacketsWindowDestroy(initial);
    IHS_SessionPacketsWindow *w = IHS_SessionPacketsWindowCreate(32);
    IHS_SessionFrame frame = {0};
    IHS_BufferInit(&frame.body, 32, 1024);
    add(w, 65534, 0xffff0000u, IHS_SessionPacketTypeUnreliable, 0);
    assert(IHS_SessionPacketsWindowPoll(w, &frame));
    IHS_SessionPacketsWindowReleaseFrame(&frame);
    add(w, 0, 0xffff8000u, IHS_SessionPacketTypeUnreliable, 0); // lost 65535
    add(w, 2, 0xffff8000u, IHS_SessionPacketTypeUnreliable, 0); // lost 1
    IHS_MediaFeedback f;
    assert(IHS_SessionPacketsWindowMediaFeedback(w, &f));
    assert(f.base == 0 && f.confirmed == 65534 && f.bitmap[0] == 5);
    assert(f.receiveTimestamp == 500 && !f.retransmitCount);
    assert(!IHS_SessionPacketsWindowPoll(w, &frame));
    add(w, 2, 0, IHS_SessionPacketTypeUnreliable, 0); // duplicate cannot change age
    assert(IHS_SessionPacketsWindowDiscard(w, 6946) == 0);
    add(w, 3, 0xffff8000u + 6946, IHS_SessionPacketTypeUnreliable, 0);
    assert(IHS_SessionPacketsWindowDiscard(w, 6946) == 0); // equality retained
    add(w, 4, 0xffff8000u + 6947, IHS_SessionPacketTypeUnreliable, 0);
    assert(IHS_SessionPacketsWindowDiscard(w, 6946) == 4); // hole and old heads
    assert(IHS_SessionPacketsWindowPoll(w, &frame));
    assert(frame.header.packetId == 3);
    IHS_SessionPacketsWindowReleaseFrame(&frame);
    IHS_SessionPacketsWindowReleaseAll(w);
    // A second frame head must never be concatenated as a continuation.
    add(w, 10, 100, IHS_SessionPacketTypeUnreliable, 1);
    add(w, 11, 100, IHS_SessionPacketTypeUnreliable, 0);
    assert(!IHS_SessionPacketsWindowPoll(w, &frame));
    IHS_SessionPacketsWindowReleaseAll(w);
    add(w, 10, 0xfffffff0u, IHS_SessionPacketTypeUnreliable, 1);
    assert(!IHS_SessionPacketsWindowPoll(w, &frame));
    add(w, 11, 0xfffffff0u, IHS_SessionPacketTypeUnreliableFrag, 0);
    assert(IHS_SessionPacketsWindowPoll(w, &frame));
    assert(frame.body.size == 2);
    IHS_SessionPacketsWindowReleaseFrame(&frame);
    // Sender timestamp wrapping must still expire a lost head.
    add(w, 13, 0xfffffff0u, IHS_SessionPacketTypeUnreliable, 0);
    add(w, 14, 0x2000, IHS_SessionPacketTypeUnreliable, 0);
    assert(IHS_SessionPacketsWindowDiscard(w, 6946) == 2);
    assert(IHS_SessionPacketsWindowPoll(w, &frame) && frame.header.packetId == 14);
    IHS_SessionPacketsWindowReleaseFrame(&frame);
    IHS_BufferClear(&frame.body, true);
    IHS_SessionPacketsWindowDestroy(w);
}
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ready = PTHREAD_COND_INITIALIZER;
static bool entered, releaseDecode;
static void init(IHS_SessionChannel *c, const void *cfg) {
    (void)cfg;
    IHS_SessionChannelDataInit(c, 32);
}
static bool start(IHS_SessionChannel *c) {
    (void)c;
    return true;
}
static void stop(IHS_SessionChannel *c) {
    (void)c;
}
static void data(IHS_SessionChannel *c, const IHS_SessionDataFrameHeader *h, IHS_Buffer *b) {
    (void)c;
    (void)h;
    (void)b;
    pthread_mutex_lock(&lock);
    entered = true;
    pthread_cond_signal(&ready);
    while (!releaseDecode)
        pthread_cond_wait(&ready, &lock);
    pthread_mutex_unlock(&lock);
}
static const IHS_SessionChannelDataClass cls = {
    .base = {.init = init,
             .deinit = IHS_SessionChannelDataDeinit,
             .received = IHS_SessionChannelDataReceived,
             .stopped = IHS_SessionChannelDataStopped,
             .instanceSize = sizeof(IHS_SessionChannelData)},
    .start = start,
    .stop = stop,
    .dataFrame = data,
};
static void deliver(IHS_SessionChannel *c, uint16_t id, uint32_t stamp) {
    IHS_SessionPacket p = {.header = {.type = IHS_SessionPacketTypeUnreliable,
                                      .packetId = id,
                                      .sendTimestamp = stamp,
                                      .receiveTimestamp = IHS_SessionPacketTimestamp()}};
    IHS_BufferInit(&p.body, 16, 16);
    IHS_BufferAppendUInt8(&p.body, k_EStreamDataPacket);
    for (unsigned i = 0; i < 13; i++)
        IHS_BufferAppendUInt8(&p.body, 0);
    c->cls->received(c, &p);
    IHS_SessionPacketClear(&p, true);
}
static void expect_feedback(IHS_SessionChannel *c, uint16_t base, uint16_t confirmed,
                            uint8_t mask, uint32_t echo) {
    uint64_t until = IHS_TimerNow() + 500;
    feedbackSize = 0;
    while (IHS_TimerNow() < until) {
        IHS_SessionChannelDataUpdateAt((void *)c, IHS_SessionPacketTimestamp());
        if (feedbackSize) {
            const uint8_t *wire = feedbackWire;
            assert(wire[0] == 10 && feedbackSize == 20);
            uint16_t b, h; uint32_t t;
            IHS_ReadUInt16LE(wire + 7, &b);
            IHS_ReadUInt16LE(wire + 17, &h);
            IHS_ReadUInt32LE(wire + 13, &t);
            assert(b == base && h == confirmed && wire[19] == mask && t == echo);
            return;
        }
        usleep(1000);
    }
    assert(!"missing expected UnreliableNack while decoder blocked");
}
/* Expired holes must publish even when the remaining head is incomplete and
 * the decoder never receives a frame. Uses the RX protocol step while the decoder waits. */
static void loss_without_frame(void) {
    IHS_Session *s = IHS_TestSessionCreate();
    s->state.connectionState = IHS_SessionConnectionStateHandshaking;
    IHS_SessionChannel *c =
        IHS_SessionChannelDataCreate(&cls, s, IHS_SessionChannelTypeDataVideo, 3, NULL);
    assert(c);
    deliver(c, 1, 1000);
    IHS_SessionPacket p = {.header = {.type = IHS_SessionPacketTypeUnreliable,
        .packetId = 3, .fragmentId = 1, .sendTimestamp = 10000,
        .receiveTimestamp = IHS_SessionPacketTimestamp()}};
    IHS_BufferInit(&p.body, 4, 4);
    IHS_BufferAppendUInt8(&p.body, k_EStreamDataPacket);
    c->cls->received(c, &p);
    IHS_SessionPacketClear(&p, true);
    IHS_SessionChannelDataUpdateAt((void *)c, IHS_SessionPacketTimestamp());
    uint32_t lost = 0;
    uint64_t until = IHS_TimerNow() + 1000;
    do {
        IHS_BaseLock(&s->base);
        lost = s->networkPackets.lost;
        IHS_BaseUnlock(&s->base);
        if (lost) break;
        usleep(1000);
    } while (IHS_TimerNow() < until);
    assert(lost == 1);
    pthread_mutex_lock(&lock);
    assert(!entered);
    pthread_mutex_unlock(&lock);
    IHS_SessionChannelStop(c);
    IHS_SessionChannelDestroy(c);
    IHS_SessionDestroy(s);
}
int main(void) {
    IHS_Init();
    window_cases();
    loss_without_frame();
    IHS_Session *s = IHS_TestSessionCreate();
    s->state.connectionState = IHS_SessionConnectionStateHandshaking;
    IHS_SessionChannel *c =
        IHS_SessionChannelDataCreate(&cls, s, IHS_SessionChannelTypeDataVideo, 3, NULL);
    assert(c);
    deliver(c, 0, 1000);
    IHS_SessionChannelDataUpdateAt((void *)c, IHS_SessionPacketTimestamp());
    pthread_mutex_lock(&lock);
    while (!entered)
        pthread_cond_wait(&ready, &lock);
    pthread_mutex_unlock(&lock);
    deliver(c, 2, 1100);
    deliver(c, 4, 1200);
    expect_feedback(c, 2, 0, 5, 1200);
    deliver(c, 1, 1000); // retransmission fills first hole
    expect_feedback(c, 4, 2, 1, 1200);
    pthread_mutex_lock(&lock);
    releaseDecode = true;
    pthread_cond_signal(&ready);
    pthread_mutex_unlock(&lock);
    IHS_SessionChannelStop(c);
    IHS_SessionChannelDestroy(c); // joins decoder before freeing dependencies
    IHS_SessionDestroy(s);
    IHS_Quit();
    return 0;
}
