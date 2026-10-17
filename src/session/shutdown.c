/* Official client connection Disconnect/Close/Update: 0x7fb724/0x7fbce0/0x7fc1c8.
 * Session authentication phases remain separate from the transport lifecycle. */
#include "session_pri.h"

IHS_TransportState IHS_SessionTransportState(const IHS_Session *s) {
    unsigned shutdown = atomic_load(&s->transport.shutdown);
    if (shutdown)
        return shutdown == 1 ? IHS_TransportClosing : IHS_TransportClosed;
    int phase = atomic_load(&s->state.connectionState);
    if (phase == IHS_SessionConnectionStateConnecting)
        return IHS_TransportConnecting;
    return phase >= IHS_SessionConnectionStateHandshaking ? IHS_TransportConnected
                                                          : IHS_TransportClosed;
}

void IHS_SessionTransportDisconnect(IHS_Session *s) {
    IHS_BaseLock(&s->base);
    if (s->transport.detached) {
        IHS_BaseUnlock(&s->base);
        return;
    }
    IHS_TransportState state = IHS_SessionTransportState(s);
    if (state == IHS_TransportConnected) {
        IHS_SessionPacket packet = {.header = {.type = IHS_SessionPacketTypeDisconnect,
                                               .srcConnectionId = s->state.connectionId,
                                               .dstConnectionId = s->state.hostConnectionId}};
        IHS_SessionPacketBodyInitialize(&packet.body, false);
        /* Socket applies the negotiated CRC. Never queue or retry type 9. */
        IHS_SessionSendPacket(s, &packet);
        IHS_SessionPacketClear(&packet, true);
    }
    if (state != IHS_TransportClosed)
        atomic_store(&s->transport.shutdown, 1);
    IHS_BaseUnlock(&s->base);
}

void IHS_SessionTransportClose(IHS_Session *s) {
    IHS_BaseLock(&s->base);
    if (!s->transport.detached) {
        IHS_SessionTransportDisconnect(s);
        s->transport.detached = true;
    }
    IHS_BaseUnlock(&s->base);
}

void IHS_SessionNotifyDisconnected(IHS_Session *s) {
    /* Application callback, distinct from the official transport delegate which
     * Close detaches. Keep one notification across overlapping exit requests. */
    IHS_BaseLock(&s->base);
    if (atomic_exchange(&s->transport.notified, true)) {
        IHS_BaseUnlock(&s->base);
        return;
    }
    const IHS_StreamSessionCallbacks *callbacks = s->callbacks.session;
    void *context = s->callbackContexts.session;
    IHS_BaseUnlock(&s->base);
    if (callbacks && callbacks->disconnected)
        callbacks->disconnected(s, context);
}

bool IHS_SessionTransportFinish(IHS_Session *s) {
    IHS_BaseLock(&s->base);
    if (atomic_load(&s->transport.shutdown) != 1) {
        IHS_BaseUnlock(&s->base);
        return false;
    }
    /* Original delegate observes state 2 and old IDs before their reset. */
    if (!s->transport.detached)
        IHS_SessionClientTransportClosed(s);
    s->state.hostConnectionId = 0;
    s->state.connectionId = IHS_SessionNextConnectionId();
    s->transport.addressCleared = true;
    atomic_store(&s->transport.shutdown, 2);
    s->transport.active = false;
    IHS_BaseUnlock(&s->base);
    return true;
}
