#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ihslib.h"
#include "common/test_session.h"
#include "session/channels/ch_control.h"

static void Ack(IHS_SessionChannel *channel, uint16_t packetId) {
    IHS_SessionPacket packet;
    memset(&packet, 0, sizeof(packet));
    packet.header.type = IHS_SessionPacketTypeACK;
    packet.header.channelId = IHS_SessionChannelIdControl;
    packet.header.packetId = packetId;
    IHS_SessionChannelReceivedPacket(channel, &packet);
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

    assert(IHS_SessionChannelControlSubmitHIDReport(channel, second, sizeof(second)));
    assert(control->hidInFlightCount == IHS_CONTROL_HID_MAX_IN_FLIGHT);
    assert(!control->hidPendingValid);
    uint16_t secondId = control->hidInFlightIds[1];
    assert(secondId != firstId);

    /* A missing ACK for firstId must not prevent a newer full snapshot from
     * advancing through the second in-flight lane. */
    assert(IHS_SessionChannelControlSubmitHIDReport(channel, third, sizeof(third)));
    assert(IHS_SessionChannelControlSubmitHIDReport(channel, latest, sizeof(latest)));
    assert(control->hidInFlightCount == IHS_CONTROL_HID_MAX_IN_FLIGHT);
    assert(control->hidPendingValid);
    assert(control->hidPending.size == sizeof(latest));
    assert(memcmp(IHS_BufferPointer(&control->hidPending), latest, sizeof(latest)) == 0);
    assert(control->hidSubmitted == 4 && control->hidCoalesced == 2 && control->hidSent == 2);

    Ack(channel, secondId);
    assert(control->hidInFlightCount == 1);
    assert(!control->hidPendingValid);
    assert(control->hidSent == 3 && control->hidAcknowledged == 1);
    assert(control->hidSuperseded == 1);
    uint16_t latestId = control->hidInFlightIds[0];
    assert(latestId != firstId && latestId != secondId);
    Ack(channel, latestId);
    assert(control->hidInFlightCount == 0);
    assert(control->hidAcknowledged == 2);
    assert(control->hidSuperseded == 1);

    IHS_SessionDestroy(session);
    IHS_Quit();
    puts("control HID admission OK");
    return 0;
}
