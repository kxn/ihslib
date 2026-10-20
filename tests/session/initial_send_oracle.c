/* Real producer -> complete worker transaction -> retained sender -> Socket.
 * Clock/final UDP are controlled. Update calls the sender component, not a
 * full real-time connection loop. */
#include "ihs_buffer.h"
#include "session/channels/ch_control.h"
#include "session/channels/ch_stats.h"
#include "session/session_pri.h"
#include "test_session.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint32_t now = 1000000;
static unsigned failIndex, error, attempts, sent;
static char *wires[16];
uint32_t __wrap_IHS_SessionPacketTimestamp(void) {
    return now;
}
bool __wrap_IHS_BaseSend(IHS_Base *base, IHS_SocketAddress address, const IHS_Buffer *wire) {
    (void)base;
    (void)address;
    assert(sent < 16);
    char *hex = malloc(wire->size * 2 + 1);
    assert(hex);
    for (size_t i = 0; i < wire->size; ++i)
        sprintf(hex + 2 * i, "%02x", IHS_BufferPointer(wire)[i]);
    wires[sent++] = hex;
    if (attempts++ == failIndex) {
        const int errors[] = {0, EAGAIN, EMSGSIZE, EBADF, EIO};
        errno = errors[error];
        return false;
    }
    return true;
}
static bool send_packet(IHS_SessionPacket *packet, void *context) {
    return IHS_SessionSendPacket(context, packet);
}
static void print(IHS_Session *s) {
    IHS_RetransmissionStats stats;
    IHS_RetransmissionGetStats(&s->retransmission, &stats, IHS_TimerNow());
    printf("{\"wire\":[");
    for (unsigned i = 0; i < sent; ++i) {
        printf("%s\"%s\"", i ? "," : "", wires[i]);
        free(wires[i]);
    }
    printf("],\"outstanding\":%u,\"next_id\":%u}", stats.outstanding, s->outgoing.reliableId[2]);
    sent = 0;
}
static void stats(IHS_Session *s, unsigned length) {
    char text[1001];
    assert(length <= 1000);
    memset(text, 'x', length);
    text[length] = 0;
    CLogMsg message = CLOG_MSG__INIT;
    message.message = text;
    assert(IHS_SessionChannelStatsSend(s->channels[2], k_EStreamStatsLogMessage, &message.base,
                                       IHS_PACKET_ID_NEXT));
}
int main(int argc, char **argv) {
    assert(argc == 5);
    bool crc = atoi(argv[1]) != 0;
    failIndex = (unsigned)atoi(argv[2]);
    error = (unsigned)atoi(argv[3]);
    unsigned trigger = (unsigned)atoi(argv[4]);
    assert(failIndex < 3 && error >= 1 && error <= 4);
    IHS_Init();
    IHS_Session *s = IHS_TestSessionCreate();
    IHS_TimerTaskStopImmediate(s->retransmission.timer);
    s->state.connectionState = IHS_SessionConnectionStateHandshaking;
    s->state.connectionId = 12;
    s->state.hostConnectionId = 34;
    s->state.crcRequired = crc;
    s->state.mtu = 512;
    s->clock.smoothedRTT = 100;
    s->outgoing.rate[0] = s->outgoing.rate[1] = 0;
    stats(s, 1000);
    assert(IHS_SessionDispatchQueued(s, false));
    assert(IHS_QueueIsEmpty(s->sendQueue));
    putchar('[');
    print(s);
    putchar(',');
    now++;
    if (trigger == 1) {
        stats(s, 0);
        assert(IHS_SessionDispatchQueued(s, false));
    } else {
        if (trigger == 2) {
            assert(failIndex > 0);
            IHS_RetransmissionAcknowledgeThrough(&s->retransmission, 2, failIndex, IHS_TimerNow());
        }
        IHS_RetransmissionProcessChannel(&s->retransmission, IHS_TimerNow(), now, 2, send_packet,
                                         s);
    }
    print(s);
    puts("]");
    IHS_SessionDestroy(s);
    IHS_Quit();
    return 0;
}
