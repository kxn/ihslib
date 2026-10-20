/* Official client phase/consumer stopping: protocol reference §65–66. */
#include "channels/ch_data.h"
#include "hid/manager.h"
#include "session_pri.h"

void IHS_SessionClientTransportClosed(IHS_Session *s) {
    if (s->networkTest.mode) {
        IHS_SessionNetworkTestClosed(s);
        s->state.connectionState = IHS_SessionConnectionStateUnconnected;
        return;
    }
    /* Original CStreamClient::OnStreamDisconnected 7b00d0: only Streaming
     * enters Stopping; all other controlled phases return to Unconnected. */
    if (s->state.connectionState == IHS_SessionConnectionStateConnected) {
        s->state.connectionState = IHS_SessionConnectionStateStopping;
        s->transport.clientPaused = false; /* original client+19, 7b0154 */
    } else {
        s->state.connectionState = IHS_SessionConnectionStateUnconnected;
    }
}

void IHS_SessionClientStopStreaming(IHS_Session *s) {
    /* Original HandleStreamStopping 7a7238: audio, video, player notice, input,
     * stats, phase0. Joining a decoder must not hold Base or the queue lock. */
    const IHS_SessionChannelType types[] = {IHS_SessionChannelTypeDataAudio,
                                            IHS_SessionChannelTypeDataVideo};
    for (unsigned i = 0; i < 2; ++i) {
        IHS_SessionChannel *channel = IHS_SessionChannelForType(s, types[i]);
        if (channel)
            IHS_SessionChannelRemove(s, channel->id);
    }
    IHS_SessionMicrophoneClose(s, false);
    IHS_SessionNotifyDisconnected(s);
    IHS_HIDManagerCloseAll(s->hidManager);
    IHS_BaseLock(&s->base);
    IHS_MediaNetworkStatsStop(&s->mediaNetworkStats);
    IHS_BaseUnlock(&s->base);
    /* Source statistics-file persistence is a separate platform consumer;
     * stopping does not invent a final protocol report. */
    s->state.connectionState = IHS_SessionConnectionStateUnconnected;
}
