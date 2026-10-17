/* Official client phase/consumer stopping: protocol reference §65–66. */
#include "channels/ch_data.h"
#include "hid/manager.h"
#include "session_pri.h"

void IHS_SessionClientTransportClosed(IHS_Session *s) {
    /* Original CStreamClient::OnStreamDisconnected 7b00d0: only Streaming
     * enters Stopping; all other controlled phases return to Unconnected. */
    s->state.connectionState = s->state.connectionState == IHS_SessionConnectionStateConnected
                                   ? IHS_SessionConnectionStateStopping
                                   : IHS_SessionConnectionStateUnconnected;
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
    IHS_SessionNotifyDisconnected(s);
    IHS_HIDManagerCloseAll(s->hidManager);
    /* Official CNetworkStats scoring/persistence is a separate platform
     * consumer; no final protocol report is invented here. */
    s->state.connectionState = IHS_SessionConnectionStateUnconnected;
}
