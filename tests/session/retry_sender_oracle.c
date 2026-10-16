/* Actual retained reliable packets -> tick eligibility -> socket serialization.
 * Only timestamp and final UDP send are doubles. No authenticated host. */
#include "ihs_buffer.h"
#include "ihs_buffer_ext.h"
#include "session/channels/ch_control.h"
#include "session/session_pri.h"
#include "test_session.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t now;
static unsigned transportResult, sent;
uint32_t __wrap_IHS_SessionPacketTimestamp(void) {
    return now;
}
bool __wrap_IHS_BaseSend(IHS_Base *base, IHS_SocketAddress address, const IHS_Buffer *wire) {
    (void)base;
    (void)address;
    if (sent++)
        putchar(',');
    putchar('"');
    const uint8_t *p = IHS_BufferPointer(wire);
    for (size_t i = 0; i < wire->size; ++i)
        printf("%02x", p[i]);
    putchar('"');
    return transportResult == 0;
}
static bool send_packet(IHS_SessionPacket *packet, void *ctx) {
    return IHS_SessionSendPacket(ctx, packet);
}
int main(int argc, char **argv) {
    assert(argc == 5);
    unsigned count = (unsigned)strtoul(argv[1], NULL, 0);
    uint32_t stamp = (uint32_t)strtoul(argv[2], NULL, 0);
    unsigned packets = (unsigned)strtoul(argv[3], NULL, 0);
    unsigned rtt = (unsigned)strtoul(argv[4], NULL, 0);
    assert(count <= 255 && packets <= 8);
    IHS_Init();
    IHS_Session *s = IHS_TestSessionCreate();
    IHS_TimerTaskStopImmediate(s->retransmission.timer);
    IHS_TimerTaskStopImmediate(((IHS_SessionChannelControl *)s->channels[1])->feedbackTimer);
    s->state.connectionId = s->state.hostConnectionId = 0;
    s->state.crcRequired = true;
    s->clock.smoothedRTT = rtt;
    s->outgoing.rate[0] = s->outgoing.rate[1] = 0;
    for (unsigned i = 0; i < packets; ++i) {
        IHS_SessionPacket p = {0};
        p.header.type = IHS_SessionPacketTypeReliable;
        p.header.channelId = 1;
        p.header.packetId = 100 + i;
        p.header.sendTimestamp = stamp;
        // Seed the retained count by recording the preceding successful initial
        // send; zero means the initial attempt has failed. Neither is emitted.
        p.header.retransmitCount = count ? count - 1 : 0;
        IHS_SessionPacketBodyInitialize(&p.body, false);
        IHS_BufferAppendUInt8(&p.body, 1);
        assert(IHS_RetransmissionTrack(&s->retransmission, &p, 0));
        IHS_RetransmissionNoteInitialSend(&s->retransmission, &p.header, count != 0, 0);
        IHS_SessionPacketClear(&p, true);
    }
    char line[80];
    while (fgets(line, sizeof(line), stdin)) {
        assert(sscanf(line, "%u %u", &now, &transportResult) == 2);
        sent = 0;
        printf("{\"sent\":[");
        size_t attempts =
            IHS_RetransmissionProcessTicks(&s->retransmission, 0, now, send_packet, s);
        printf("],\"attempts\":%zu}\n", attempts);
    }
    IHS_SessionDestroy(s);
    IHS_Quit();
    return 0;
}
