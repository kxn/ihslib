/* Complete publication, future-frame isolation and resource-failure rollback. */
#include "ihs_buffer.h"
#include "session/channels/ch_control.h"
#include "session/channels/ch_stats.h"
#include "session/session_pri.h"
#include "test_session.h"
#include <assert.h>
#include <errno.h>
#include <string.h>
static unsigned attempts, ids[8], failTrack, tracks, failItem, items;
static bool failQueue, writeFail;
uint32_t __wrap_IHS_SessionPacketTimestamp(void) {
    return 1000000;
}
bool __wrap_IHS_BaseSend(IHS_Base *base, IHS_SocketAddress address, const IHS_Buffer *wire) {
    (void)base;
    (void)address;
    const uint8_t *p = IHS_BufferPointer(wire);
    if ((p[0] & 127) == 9)
        return true;
    assert(attempts < 8);
    ids[attempts++] = p[7] | (unsigned)p[8] << 8;
    errno = EAGAIN;
    return !writeFail;
}
IHS_Queue *__real_IHS_QueueCreate(size_t);
IHS_Queue *__wrap_IHS_QueueCreate(size_t size) {
    return failQueue ? NULL : __real_IHS_QueueCreate(size);
}
IHS_QueueItem *__real_IHS_QueueItemObtain(IHS_Queue *);
IHS_QueueItem *__wrap_IHS_QueueItemObtain(IHS_Queue *q) {
    return failItem && ++items == failItem ? NULL : __real_IHS_QueueItemObtain(q);
}
bool __real_IHS_RetransmissionTrack(IHS_SessionRetransmission *, const IHS_SessionPacket *,
                                    uint64_t);
bool __wrap_IHS_RetransmissionTrack(IHS_SessionRetransmission *r, const IHS_SessionPacket *p,
                                    uint64_t t) {
    return failTrack && ++tracks == failTrack ? false : __real_IHS_RetransmissionTrack(r, p, t);
}
static IHS_Session *create(void) {
    attempts = tracks = items = failTrack = failItem = 0;
    failQueue = writeFail = false;
    IHS_Session *s = IHS_TestSessionCreate();
    IHS_TimerTaskStopImmediate(s->retransmission.timer);
    IHS_TimerTaskStopImmediate(((IHS_SessionChannelControl *)s->channels[1])->feedbackTimer);
    s->state.connectionState = IHS_SessionConnectionStateHandshaking;
    s->state.mtu = 512;
    s->outgoing.rate[0] = s->outgoing.rate[1] = 0;
    return s;
}
static bool stats(IHS_Session *s, unsigned length) {
    char text[1001];
    assert(length <= 1000);
    memset(text, 'x', length);
    text[length] = 0;
    CLogMsg msg = CLOG_MSG__INIT;
    msg.message = text;
    return IHS_SessionChannelStatsSend(s->channels[2], k_EStreamStatsLogMessage, &msg.base,
                                       IHS_PACKET_ID_NEXT);
}
static bool send_packet(IHS_SessionPacket *packet, void *ctx) {
    return IHS_SessionSendPacket(ctx, packet);
}
int main(void) {
    IHS_Init();
    IHS_Session *s = create();
    assert(stats(s, 1000) && stats(s, 0));
    writeFail = true;
    assert(IHS_SessionDispatchQueued(s, false));
    assert(attempts == 1 && ids[0] == 0);
    writeFail = false;
    IHS_RetransmissionProcessChannel(&s->retransmission, IHS_TimerNow(), 1000000, 2, send_packet,
                                     s);
    assert(attempts == 4 && ids[1] == 0 && ids[2] == 1 && ids[3] == 2);
    /* A peer ACK beyond the active tail is clamped to the actual window; the
     * queued next frame has not entered that window and must survive. */
    assert(IHS_RetransmissionAcknowledgeThrough(&s->retransmission, 2, 100, IHS_TimerNow()) == 3);
    assert(!IHS_RetransmissionAcknowledge(&s->retransmission, 2, 3, 0, IHS_TimerNow()));
    assert(IHS_RetransmissionIsTracked(&s->retransmission, 2, 3, 0));
    assert(IHS_SessionDispatchQueued(s, false));
    assert(attempts == 5 && ids[4] == 3 && IHS_QueueIsEmpty(s->sendQueue));
    assert(!IHS_SessionDispatchQueued(s, false));
    IHS_SessionDestroy(s);
    for (unsigned mode = 0; mode < 3; ++mode) {
        s = create();
        if (mode == 0)
            failQueue = true;
        if (mode == 1)
            failTrack = 2;
        if (mode == 2)
            failItem = 2;
        assert(!stats(s, 1000));
        failQueue = false;
        failTrack = failItem = 0;
        IHS_RetransmissionStats state;
        IHS_RetransmissionGetStats(&s->retransmission, &state, IHS_TimerNow());
        assert(!state.outstanding && !state.tracked && !attempts);
        assert(!s->sendBatch && IHS_QueueIsEmpty(s->sendQueue));
        assert(s->base.interrupted && IHS_SessionTransportState(s) == IHS_TransportClosing);
        IHS_SessionDestroy(s);
    }
    IHS_Quit();
    return 0;
}
