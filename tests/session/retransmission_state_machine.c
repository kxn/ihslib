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

typedef struct {
    IHS_SessionRetransmission *r;
    uint16_t ids[8];
    uint8_t counts[8];
    unsigned size, mode;
} FailureCapture;
static bool failure_send(IHS_SessionPacket *p, void *ctx) {
    FailureCapture *c = ctx;
    assert(c->size < 8);
    c->ids[c->size] = p->header.packetId;
    c->counts[c->size++] = p->header.retransmitCount;
    if (c->mode == 1 && p->header.packetId == 10) return false;
    if (c->mode == 2 && p->header.packetId == 10) {
        assert(IHS_RetransmissionAcknowledge(c->r, 1, 10, 0, 25));
        assert(IHS_RetransmissionAcknowledge(c->r, 1, 11, 0, 25));
    }
    if (c->mode == 3 && p->header.packetId == 10)
        assert(IHS_RetransmissionNack(c->r, 1, 10, 0, 25));
    return true;
}
static void failure_and_feedback(void) {
    for (unsigned mode = 1; mode <= 3; ++mode) {
        IHS_SessionRetransmission r;
        IHS_RetransmissionInit(&r, NULL);
        for (unsigned i = 0; i < 3; ++i) {
            IHS_SessionPacket p;
            InitPacket(&p, i == 2 ? 20 : 10 + i, 0, 7);
            p.header.channelId = i == 2 ? 2 : 1;
            assert(IHS_RetransmissionTrack(&r, &p, 0));
            IHS_RetransmissionNoteInitialSend(&r, &p.header, true, 0);
            IHS_SessionPacketClear(&p, true);
        }
        FailureCapture c = {.r = &r, .mode = mode};
        assert(IHS_RetransmissionProcessAt(&r, 25, failure_send, &c) == (mode == 3 ? 3 : 2));
        assert(c.ids[0] == 10 && c.ids[c.size - 1] == 20);
        if (mode == 1) {
            // Failure stops this channel, but neither advances nor delays 11.
            c.mode = 0;
            assert(IHS_RetransmissionProcessAt(&r, 26, failure_send, &c) == 1);
            assert(c.ids[2] == 11 && c.counts[2] == 1);
            assert(IHS_RetransmissionNack(&r, 1, 10, 0, 26));
            assert(IHS_RetransmissionProcessAt(&r, 26, failure_send, &c) == 1);
            assert(c.ids[3] == 10 && c.counts[3] == 1);
        } else if (mode == 2) {
            // ACK can release the current and a not-yet-visited snapshot item.
            assert(!IHS_RetransmissionIsTracked(&r, 1, 10, 0));
            assert(!IHS_RetransmissionIsTracked(&r, 1, 11, 0));
        } else {
            // Feedback during I/O is not overwritten by completion of that send.
            c.mode = 0;
            assert(IHS_RetransmissionProcessAt(&r, 25, failure_send, &c) == 1);
            assert(c.ids[3] == 10 && c.counts[3] == 2);
        }
        IHS_RetransmissionDeinit(&r);
    }
}
int main(void) {
    failure_and_feedback();
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

    /* No give-up: an unacked reliable packet retransmits forever until ACKed
     * or NACK-released, matching the official client (libmain 0x7f8ac4 has no
     * retire path). A delayed retransmit keeps a valid sequence number because
     * the peer delivers encrypted frames strictly in order. */
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
    /* Past the former 3200 ms give-up point the packet is still tracked and
     * still retransmitted (retry cadence: next due at 3225 ms). */
    assert(IHS_RetransmissionProcessAt(&retransmission, 3225, CaptureSend, &capture) == 1);
    IHS_RetransmissionGetStats(&retransmission, &stats, 3200);
    assert(stats.giveUps == 0);
    assert(stats.outstanding == 1);
    /* ACK still releases it whenever it arrives. */
    assert(IHS_RetransmissionAcknowledge(&retransmission,
                                         IHS_SessionChannelIdControl, 43, 0, 3300));
    IHS_RetransmissionGetStats(&retransmission, &stats, 3300);
    assert(stats.outstanding == 0);
    assert(stats.acknowledged == 2);

    /* Supersede cannot retire unconfirmed ciphertext: the transport has
     * no permission to leave a hole, even if a newer full report exists. */
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
    assert(stats.outstanding == 1);
    assert(stats.acknowledged == 2);
    assert(stats.superseded == 0);
    assert(stats.giveUps == 0);
    IHS_SessionPacketClear(&packet, true);
    IHS_RetransmissionDeinit(&retransmission);
    puts("retransmission state machine OK");
    return 0;
}
