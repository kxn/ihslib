/* Official ARM64 evidence: Client::Pause/Resume 0x7b02bc/0x7b0384,
 * connection OnStreamResumed 0x7fb99c, timeout Update 0x7fc43c.
 * All protocol state and queued actions run on the existing receive worker. */
#include "channels/ch_data.h"
#include "session_pri.h"

void IHS_SessionSetForeground(IHS_Session *s, bool foreground) {
    if (!s)
        return;
    unsigned generation = atomic_load(&s->transport.lifecycleGeneration);
    /* Every change increments the generation; parity records the requested
     * state. Background -> foreground between worker ticks keeps both edges. */
    while ((generation & 1u) != (unsigned)!foreground) {
        if (atomic_compare_exchange_weak(&s->transport.lifecycleGeneration, &generation,
                                         generation + 1u))
            break;
    }
}

void IHS_SessionTransportBegin(IHS_Session *s, uint32_t now) {
    s->transport.updateTime = now;
    s->transport.lastReceive = now;
    s->transport.lastConnect = 0;
    /* Constructor 0x7fb28c..90 seeds the actual ProcessTimestamp watermark. */
    IHS_MutexLock(s->clock.lock);
    s->clock.lastEcho = now;
    s->clock.haveEcho = true;
    IHS_MutexUnlock(s->clock.lock);
    s->transport.active = true;
}

bool IHS_SessionTransportSendConnect(IHS_Session *s) {
    /* Official client BConnect -> Connect -> SendConnectPacket 0x7fb84c.
     * Fresh packets, count/IDs zero except local ID, cookie but no CRC tail.
     * Connect itself does not write the periodic lastConnect watermark. */
    static const uint8_t cookie[] = {0xc7, 0x3d, 0x8f, 0x3c};
    IHS_SessionPacket packet = {.header = {.type = IHS_SessionPacketTypeConnect,
                                           .srcConnectionId = s->state.connectionId}};
    IHS_SessionPacketBodyInitialize(&packet.body, false);
    IHS_BufferAppendMem(&packet.body, cookie, sizeof(cookie));
    bool sent = IHS_SessionSendPacket(s, &packet);
    IHS_SessionPacketClear(&packet, true);
    return sent;
}

static void ChangeForeground(IHS_Session *s, bool foreground) {
    int phase = s->state.connectionState;
    bool connected = phase >= IHS_SessionConnectionStateHandshaking && !s->transport.timedOut &&
                     !s->base.interrupted;
    if (!foreground) {
        if (s->transport.clientPaused)
            return;
        if (connected) {
            if (phase == IHS_SessionConnectionStateConnected) {
                CPauseMsg message = CPAUSE_MSG__INIT;
                IHS_SessionSendControlMessage(s, k_EStreamControlPause,
                                              (const ProtobufCMessage *)&message);
            }
            s->transport.connectionPaused = true;
        }
        s->transport.clientPaused = true;
    } else {
        if (!s->transport.clientPaused)
            return;
        if (connected) {
            s->transport.connectionPaused = false;
            /* Use connection's most recent update time, not a new wall clock
             * sample in the UI event handler (OnStreamResumed +48 -> +52/+56). */
            s->transport.lastReceive = s->transport.updateTime;
            IHS_MutexLock(s->clock.lock);
            s->clock.lastEcho = s->transport.updateTime;
            s->clock.haveEcho = true;
            IHS_MutexUnlock(s->clock.lock);
            if (phase == IHS_SessionConnectionStateConnected) {
                CResumeMsg message = CRESUME_MSG__INIT;
                IHS_SessionSendControlMessage(s, k_EStreamControlResume,
                                              (const ProtobufCMessage *)&message);
                const IHS_SessionChannelType types[] = {IHS_SessionChannelTypeDataAudio,
                                                        IHS_SessionChannelTypeDataVideo};
                for (unsigned i = 0; i < 2; ++i) {
                    IHS_SessionChannel *channel = IHS_SessionChannelForType(s, types[i]);
                    /* Official data channel IDs start at zero before Start*.
                     * Sending an empty DataLost is not a decoder reset. */
                    if (!channel)
                        channel = s->channels[IHS_SessionChannelIdDiscovery];
                    IHS_SessionChannelDataLost(channel);
                }
            }
        }
        s->transport.clientPaused = false;
    }
}

void IHS_SessionTransportUpdate(IHS_Session *s, uint32_t now) {
    unsigned requested = atomic_load(&s->transport.lifecycleGeneration);
    /* Bounded per tick while preserving every queued edge for later ticks. */
    for (unsigned count = 0; count < 32 && s->transport.appliedGeneration != requested; ++count) {
        unsigned generation = ++s->transport.appliedGeneration;
        ChangeForeground(s, !(generation & 1u));
    }
    s->transport.updateTime = now;
    if (!s->transport.active || s->transport.timedOut)
        return;
    if (s->state.connectionState == IHS_SessionConnectionStateConnecting &&
        (!s->transport.lastConnect || (uint32_t)(now - s->transport.lastConnect) >= 6553u)) {
        /* Update 0x7fc190..1b8: attempt before the lease check, and write the
         * Update timestamp even on EAGAIN/hard failure; no reliable RTO window. */
        IHS_SessionTransportSendConnect(s);
        s->transport.lastConnect = now;
    }
    uint32_t timeout = (s->transport.connectionPaused ? 180u : 15u) * 65536u;
    if ((uint32_t)(now - s->transport.lastReceive) >= timeout) {
        s->transport.timedOut = true;
        s->transport.active = false;
        IHS_SessionLog(s, IHS_LogLevelWarn, "Session", "Connection timed out (%u seconds)",
                       timeout / 65536u);
        /* A local timeout is not evidence that the host stopped the game. */
        IHS_SessionDisconnect(s);
    }
}
