#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "session/retransmission.h"
#include "ihs_buffer_ext.h"

typedef struct SendCapture {
    unsigned int calls;
    uint16_t packetId;
    int16_t fragmentId;
    uint8_t payload;
    uint8_t retransmitCount;
    bool result;
} SendCapture;

static bool CaptureSend(IHS_SessionPacket *packet, void *context) {
    SendCapture *capture = context;
    capture->calls++;
    capture->packetId = packet->header.packetId;
    capture->fragmentId = packet->header.fragmentId;
    capture->payload = *IHS_BufferPointer(&packet->body);
    capture->retransmitCount = packet->header.retransmitCount;
    return capture->result;
}

static void InitPacket(IHS_SessionPacket *packet, uint16_t packetId,
                       int16_t fragmentId, uint8_t payload) {
    memset(packet, 0, sizeof(*packet));
    packet->header.hasCrc = true;
    packet->header.type = IHS_SessionPacketTypeReliable;
    packet->header.channelId = IHS_SessionChannelIdControl;
    packet->header.packetId = packetId;
    packet->header.fragmentId = fragmentId;
    IHS_SessionPacketBodyInitialize(&packet->body, true);
    IHS_BufferAppendUInt8(&packet->body, payload);
}

int main(void) {
    IHS_SessionRetransmission retransmission;
    IHS_RetransmissionInit(&retransmission, NULL);

    IHS_SessionPacket packet;
    InitPacket(&packet, 42, 3, 0xa5);
    assert(IHS_RetransmissionTrack(&retransmission, &packet, 100));

    IHS_RetransmissionStats stats;
    IHS_RetransmissionGetStats(&retransmission, &stats, 105);
    assert(stats.tracked == 1);
    assert(stats.outstanding == 1);
    assert(stats.oldestOutstandingMs == 5);
    assert(stats.oldestChannelId == IHS_SessionChannelIdControl);
    assert(stats.oldestPacketId == 42 && stats.oldestFragmentId == 3);
    assert(stats.oldestRetryCount == 0);

    SendCapture capture = {.result = true};
    /* Tracking must not arm a retry before the queued initial send completes. */
    assert(IHS_RetransmissionProcessAt(&retransmission, 1000, CaptureSend, &capture) == 0);
    IHS_RetransmissionNoteInitialSend(&retransmission, &packet.header, true, 1000);
    assert(IHS_RetransmissionProcessAt(&retransmission, 1024, CaptureSend, &capture) == 0);
    assert(IHS_RetransmissionProcessAt(&retransmission, 1025, CaptureSend, &capture) == 1);
    assert(capture.calls == 1);
    assert(capture.packetId == 42 && capture.fragmentId == 3);
    assert(capture.payload == 0xa5 && capture.retransmitCount == 1);

    /* ACK identity includes fragmentId. A neighbouring fragment cannot release it. */
    assert(!IHS_RetransmissionAcknowledge(&retransmission,
                                          IHS_SessionChannelIdControl, 42, 2, 1026));
    assert(IHS_RetransmissionNack(&retransmission,
                                  IHS_SessionChannelIdControl, 42, 3, 1026));
    assert(IHS_RetransmissionProcessAt(&retransmission, 1026, CaptureSend, &capture) == 1);
    assert(capture.retransmitCount == 2);
    assert(IHS_RetransmissionAcknowledge(&retransmission,
                                         IHS_SessionChannelIdControl, 42, 3, 1030));
    assert(!IHS_RetransmissionAcknowledge(&retransmission,
                                          IHS_SessionChannelIdControl, 42, 3, 1031));

    /* An unacked reliable packet is retired after a bounded give-up window:
     * once the peer resyncs its decrypt sequence past the gap, retransmitted
     * copies are stale replays the peer will silently drop, so they can never
     * be ACKed. Field evidence: packet 1875 retransmitted 189 s, 8000+ copies.
     * State convergence relies on the periodic full-state HID heartbeat. */
    IHS_SessionPacketClear(&packet, true);
    InitPacket(&packet, 43, 0, 0x5a);
    assert(IHS_RetransmissionTrack(&retransmission, &packet, 200));
    IHS_RetransmissionNoteInitialSend(&retransmission, &packet.header, true, 200);
    for (uint64_t now = 225; now <= 3199; now += 100) {
        IHS_RetransmissionProcessAt(&retransmission, now, CaptureSend, &capture);
    }
    IHS_RetransmissionGetStats(&retransmission, &stats, 3199);
    assert(stats.retries > 20);
    assert(stats.outstanding == 1);
    /* At 3200 ms the give-up window closes: retired without pretending an ACK. */
    assert(IHS_RetransmissionProcessAt(&retransmission, 3200, CaptureSend, &capture) == 0);
    IHS_RetransmissionGetStats(&retransmission, &stats, 3200);
    assert(stats.giveUps == 1);
    assert(stats.outstanding == 0);
    assert(!IHS_RetransmissionAcknowledge(&retransmission,
                                          IHS_SessionChannelIdControl, 43, 0, 3300));
    IHS_RetransmissionGetStats(&retransmission, &stats, 3300);
    assert(stats.outstanding == 0);
    assert(stats.acknowledged == 1);

    /* A newer full-state message may supersede an older one. Preserve three
     * gap-filling retries, then retire it without pretending an ACK arrived. */
    IHS_SessionPacketClear(&packet, true);
    InitPacket(&packet, 44, 0, 0xc3);
    assert(IHS_RetransmissionTrack(&retransmission, &packet, 6000));
    IHS_RetransmissionNoteInitialSend(&retransmission, &packet.header, true, 6000);
    assert(IHS_RetransmissionSupersede(&retransmission,
                                       IHS_SessionChannelIdControl, 44, 0));
    assert(IHS_RetransmissionProcessAt(&retransmission, 6025, CaptureSend, &capture) == 1);
    assert(capture.retransmitCount == 1);
    assert(IHS_RetransmissionProcessAt(&retransmission, 6075, CaptureSend, &capture) == 1);
    assert(capture.retransmitCount == 2);
    assert(IHS_RetransmissionProcessAt(&retransmission, 6175, CaptureSend, &capture) == 1);
    assert(capture.retransmitCount == 3);
    assert(IHS_RetransmissionProcessAt(&retransmission, 6176, CaptureSend, &capture) == 0);
    IHS_RetransmissionGetStats(&retransmission, &stats, 6176);
    assert(stats.outstanding == 0);
    assert(stats.acknowledged == 1);
    assert(stats.superseded == 1);
    assert(stats.giveUps == 1);
    IHS_SessionPacketClear(&packet, true);
    IHS_RetransmissionDeinit(&retransmission);
    puts("retransmission state machine OK");
    return 0;
}
