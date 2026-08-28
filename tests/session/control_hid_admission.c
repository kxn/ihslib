#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ihslib.h"
#include "common/test_session.h"
#include "session/channels/ch_control.h"
#include "session/retransmission.h"

static void Ack(IHS_SessionChannel *channel, uint16_t packetId) {
    IHS_SessionPacket packet;
    memset(&packet, 0, sizeof(packet));
    packet.header.type = IHS_SessionPacketTypeACK;
    packet.header.channelId = IHS_SessionChannelIdControl;
    packet.header.packetId = packetId;
    IHS_SessionChannelReceivedPacket(channel, &packet);
}

static bool DropSend(IHS_SessionPacket *packet, void *context) {
    (void) packet;
    (void) context;
    return true;
}

int main(void) {
    IHS_Init();
    IHS_Session *session = IHS_TestSessionCreate();
    /* This test drives queues synchronously; stop the production retry tick. */
    IHS_TimerTaskStopImmediate(session->retransmission.timer);
    session->state.connectionState = IHS_SessionConnectionStateConnected;

    IHS_SessionChannel *channel = IHS_SessionChannelFor(session, IHS_SessionChannelIdControl);
    IHS_SessionChannelControl *control = (IHS_SessionChannelControl *) channel;
    const uint8_t first[] = {1, 2, 3};
    const uint8_t second[] = {4, 5, 6};
    const uint8_t third[] = {7, 8, 9};
    const uint8_t latest[] = {10, 11, 12};

    assert(IHS_SessionChannelControlSubmitHIDReport(channel, first, sizeof(first)));
    assert(control->hidInFlightCount == 1);
    assert(!control->hidPendingValid);
    uint16_t firstId = control->hidInFlightIds[0];

    /* With a single in-flight slot (matching the official client's serial
     * sends), later submits coalesce into the pending full snapshot: the host
     * must never see out-of-order input reports. */
    assert(IHS_SessionChannelControlSubmitHIDReport(channel, second, sizeof(second)));
    assert(control->hidInFlightCount == 1);
    assert(control->hidPendingValid);
    assert(control->hidPending.size == sizeof(second));
    assert(IHS_SessionChannelControlSubmitHIDReport(channel, third, sizeof(third)));
    assert(IHS_SessionChannelControlSubmitHIDReport(channel, latest, sizeof(latest)));
    assert(control->hidInFlightCount == 1);
    assert(control->hidPendingValid);
    assert(control->hidPending.size == sizeof(latest));
    assert(memcmp(IHS_BufferPointer(&control->hidPending), latest, sizeof(latest)) == 0);
    assert(control->hidSubmitted == 4 && control->hidCoalesced == 3 && control->hidSent == 1);

    /* The ACK frees the slot; the pending snapshot - the newest full state -
     * goes out immediately, so no state older than one RTT is ever applied. */
    Ack(channel, firstId);
    assert(control->hidInFlightCount == 1);
    assert(!control->hidPendingValid);
    assert(control->hidSent == 2 && control->hidAcknowledged == 1);
    uint16_t latestId = control->hidInFlightIds[0];
    assert(latestId != firstId);
    Ack(channel, latestId);
    assert(control->hidInFlightCount == 0);
    assert(control->hidAcknowledged == 2);
    assert(control->hidSuperseded == 0);

    /* A give-up-retired report (ACK can never arrive once the peer resynced
     * past it) must release its in-flight slot: the next submit goes out
     * instead of coalescing forever behind a dead packet. */
    assert(IHS_SessionChannelControlSubmitHIDReport(channel, first, sizeof(first)));
    assert(control->hidInFlightCount == 1);
    uint16_t stuckId = control->hidInFlightIds[0];
    assert(IHS_RetransmissionIsTracked(&session->retransmission,
                                       IHS_SessionChannelIdControl, stuckId, 0));
    /* The test session has no send worker: note the initial send manually,
     * exactly as the production send queue would after handing the packet off. */
    IHS_SessionPacketHeader stuckHeader;
    memset(&stuckHeader, 0, sizeof(stuckHeader));
    stuckHeader.channelId = IHS_SessionChannelIdControl;
    stuckHeader.packetId = stuckId;
    stuckHeader.fragmentId = 0;
    uint64_t notedMs = IHS_TimerNow();
    IHS_RetransmissionNoteInitialSend(&session->retransmission, &stuckHeader, true, notedMs);
    /* Run the retransmission sweep past the 3 s give-up window. */
    for (uint64_t now = notedMs; now <= notedMs + 3200; now += 100) {
        IHS_RetransmissionProcessAt(&session->retransmission, now, DropSend, NULL);
    }
    assert(!IHS_RetransmissionIsTracked(&session->retransmission,
                                        IHS_SessionChannelIdControl, stuckId, 0));
    assert(IHS_SessionChannelControlSubmitHIDReport(channel, second, sizeof(second)));
    assert(control->hidInFlightCount == 1);
    assert(control->hidInFlightIds[0] != stuckId);
    assert(!control->hidPendingValid);

    IHS_SessionDestroy(session);
    IHS_Quit();
    puts("control HID admission OK");
    return 0;
}
