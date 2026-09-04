#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ihslib.h"
#include "common/test_session.h"
#include "session/channels/ch_control.h"
#include "session/retransmission.h"

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

    /* Official input path: every submit goes out immediately, one message per
     * batch of device deltas; there is no in-flight window and no coalescing.
     * Reliable delivery is the transport's job (retransmit until ACK/NACK). */
    assert(IHS_SessionChannelControlSubmitHIDReport(channel, first, sizeof(first)));
    assert(control->hidSent == 1);
    assert(IHS_SessionChannelControlSubmitHIDReport(channel, second, sizeof(second)));
    assert(control->hidSent == 2);
    assert(control->hidSubmitted == 2);
    assert(control->hidCoalesced == 0);

    /* Transport keeps tracking both packets until they are ACKed, and no
     * give-up retirement exists: past 3 s both are still outstanding and
     * still retransmitted. */
    for (uint64_t now = 0; now <= 3200; now += 100) {
        IHS_RetransmissionProcessAt(&session->retransmission, now, DropSend, NULL);
    }
    assert(session->retransmission.stats.giveUps == 0);
    assert(session->retransmission.stats.outstanding >= 2);

    IHS_SessionDestroy(session);
    IHS_Quit();
    puts("control HID admission OK");
    return 0;
}
