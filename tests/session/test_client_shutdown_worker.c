/* Actual RX/TX and audio/video workers. A reliable frame is handed to the
 * control FIFO, then a controlled transport close/retirement precedes drain.
 * Decoder stop joins must finish before the application notice, without Base
 * held. A decoder-originated cancellation also exercises table ownership. */
#include "ihs_buffer.h"
#include "ihs_buffer_ext.h"
#include "ihslib.h"
#include "session/channels/ch_control.h"
#include "session/channels/ch_data.h"
#include "session/session_pri.h"
#include "test_session.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>

static IHS_Session *session;
static unsigned mode;
static bool crc;
static atomic_uint now, stage, handshakeId, goodbyes, stopOrder, notified, saved;
static atomic_bool handshake, gate, entered, triggered, finished, started[2];
uint32_t __wrap_IHS_SessionPacketTimestamp(void) { return atomic_load(&now); }
static void data_init(IHS_SessionChannel *ch, const void *unused) {
    (void)unused;
    assert(IHS_SessionChannelDataInit(ch, 32));
}
static bool data_start(IHS_SessionChannel *ch) {
    atomic_store(&started[ch->type == IHS_SessionChannelTypeDataVideo], true);
    return true;
}
static void data_stop(IHS_SessionChannel *ch) {
    unsigned which = ch->type == IHS_SessionChannelTypeDataVideo;
    /* Stop is on the real data worker being joined by the RX owner. */
    IHS_BaseLock(&session->base);
    IHS_BaseUnlock(&session->base);
    if (!which && mode == 2)
        IHS_SessionDisconnect(session);
    assert(atomic_fetch_add(&stopOrder, 1) == which);
}
static const IHS_SessionChannelDataClass dataClass = {
    .base = {.instanceSize = sizeof(IHS_SessionChannelData),
             .init = data_init,
             .deinit = IHS_SessionChannelDataDeinit,
             .stopped = IHS_SessionChannelDataStopped,
             .received = IHS_SessionChannelDataReceived},
    .start = data_start,
    .stop = data_stop};
static void *acquire_base(void *unused) {
    (void)unused;
    IHS_BaseLock(&session->base);
    IHS_BaseUnlock(&session->base);
    return NULL;
}
static void stopped(IHS_Session *s, void *ctx) {
    (void)ctx;
    assert(atomic_load(&stopOrder) == 2);
    assert(atomic_load(&notified) == (mode == 3 ? 0u : 1u));
    assert(atomic_load(&saved) == (mode == 3 ? 0u : 1u));
    assert(s->state.connectionState ==
           (mode == 3 ? IHS_SessionConnectionStateConnected : IHS_SessionConnectionStateStopping));
    assert(IHS_SessionTransportState(s) ==
           (mode == 3 ? IHS_TransportClosing : IHS_TransportClosed));
    assert(s->state.hostConnectionId == (mode == 3 ? 34u : 0u));
    if (mode == 3) {
        IHS_SessionChannelControl *control = (void *)s->channels[1];
        assert(control->incomingCount == 1 && !s->transport.detached);
    }
    assert(!IHS_SessionChannelForType(s, IHS_SessionChannelTypeDataAudio));
    assert(!IHS_SessionChannelForType(s, IHS_SessionChannelTypeDataVideo));
    assert(atomic_fetch_add(&stopOrder, 1) == 2);
    pthread_t p;
    assert(!pthread_create(&p, NULL, acquire_base, NULL));
    assert(!pthread_join(p, NULL));
    atomic_store(&finished, true);
}
static void changed(IHS_Session *s, const uint8_t *wire, size_t size, bool local, void *ctx) {
    (void)wire;
    (void)size;
    (void)ctx;
    assert(local && !atomic_load(&stopOrder));
    assert(IHS_SessionTransportState(s) ==
           (mode == 1 ? IHS_TransportClosed : IHS_TransportClosing));
    assert(s->state.connectionState ==
           (mode == 1 ? IHS_SessionConnectionStateStopping : IHS_SessionConnectionStateConnected));
    assert(atomic_fetch_add(&notified, 1) == 0);
}
static void save(IHS_Session *s, const uint8_t *wire, size_t size, void *ctx) {
    (void)ctx;
    assert(atomic_load(&notified) == 1 && !atomic_load(&stopOrder));
    CStreamingClientConfig *p = cstreaming_client_config__unpack(NULL, size, wire);
    assert(p);
    assert(p->has_enable_unreliable_fec && p->enable_unreliable_fec &&
           p->desired_bitrate_kbps == 12345);
    assert(s->clientConfig->enable_unreliable_fec);
    cstreaming_client_config__free_unpacked(p, NULL);
    assert(atomic_fetch_add(&saved, 1) == 0);
}
bool __wrap_IHS_BaseSend(IHS_Base *base, IHS_SocketAddress address, const IHS_Buffer *wire) {
    (void)base;
    (void)address;
    const uint8_t *p = IHS_BufferPointer(wire);
    if ((p[0] & 127) == 5 && p[4] == 1) {
        atomic_store(&handshakeId, p[7] | (unsigned)p[8] << 8);
        atomic_store(&handshake, true);
    }
    if ((p[0] & 127) == 9)
        assert(atomic_fetch_add(&goodbyes, 1) == 0);
    return true;
}
void __real_IHS_SessionReliableUpdateChannelAt(IHS_Session *, uint8_t, uint32_t);
void __wrap_IHS_SessionReliableUpdateChannelAt(IHS_Session *s, uint8_t ch, uint32_t tick) {
    __real_IHS_SessionReliableUpdateChannelAt(s, ch, tick);
    IHS_SessionChannelControl *control = (void *)s->channels[1];
    if (ch == 1 && control->incomingCount && !atomic_exchange(&triggered, true)) {
        assert(s->state.connectionState == IHS_SessionConnectionStateConnected &&
               !atomic_load(&saved));
        if (mode == 3) {
            IHS_SessionDisconnect(s);
            return;
        }
        IHS_SessionTransportDisconnect(s);
        if (mode == 1)
            assert(IHS_SessionTransportFinish(s));
    }
}
static int incoming(IHS_UDPPacket *rx, unsigned type, unsigned id) {
    IHS_SessionPacket p = {.header = {.type = type,
                                      .channelId = type == 2 ? 0 : 1,
                                      .packetId = id,
                                      .srcConnectionId = 34,
                                      .dstConnectionId = session->state.connectionId,
                                      .hasCrc = type != 2 && crc,
                                      .sendTimestamp = 900000}};
    IHS_SessionPacketBodyInitialize(&p.body, p.header.hasCrc);
    if (type == 5) {
        const uint8_t body[] = {0x0a, 0x06, 0x30, 0xb9, 0x60, 0xc8, 0x01, 0x01};
        uint8_t cipher[128];
        size_t size = sizeof(cipher);
        assert(!IHS_SessionFrameEncrypt(session, body, sizeof(body), cipher, &size, 0));
        IHS_BufferAppendUInt8(&p.body, 99);
        IHS_BufferAppendMem(&p.body, cipher, size);
    } else
        IHS_BufferAppendUInt32LE(&p.body, 1000000);
    IHS_SessionPacketPopulateBuffer(&p);
    rx->address = session->info.address;
    IHS_BufferAppendMem(&rx->buffer, p.body.data, IHS_SessionPacketSize(&p));
    IHS_SessionPacketClear(&p, true);
    return 1;
}
int __wrap_IHS_UDPSocketWaitReadable(IHS_UDPSocket *socket, uint32_t ms) {
    (void)socket;
    assert(ms == 0 || ms == 2);
    return 1;
}
int __wrap_IHS_UDPSocketReceive(IHS_UDPSocket *socket, IHS_UDPPacket *rx) {
    (void)socket;
    switch (atomic_load(&stage)) {
    case 0:
        atomic_store(&stage, 1);
        return incoming(rx, 2, 0);
    case 1:
        atomic_store(&stage, 2);
        return 0;
    case 2:
        if (!atomic_load(&handshake)) {
            usleep(1000);
            return 0;
        }
        atomic_store(&stage, 3);
        return incoming(rx, 7, atomic_load(&handshakeId));
    case 3:
        atomic_store(&stage, 4);
        return 0;
    case 4:
        atomic_store(&entered, true);
        while (!atomic_load(&gate) && !session->base.interrupted)
            usleep(1000);
        atomic_store(&stage, 5);
        return incoming(rx, 5, 0);
    case 5:
        atomic_store(&stage, 6);
        return 0;
    default:
        usleep(1000);
        return 0;
    }
}
static void wait_bool(atomic_bool *p) {
    uint64_t end = IHS_TimerNow() + 3000;
    while (!atomic_load(p) && IHS_TimerNow() < end)
        usleep(1000);
    assert(atomic_load(p));
}
int main(void) {
    IHS_Init();
    for (mode = 0; mode < 4; ++mode)
        for (unsigned c = 0; c < 2; ++c)
            for (unsigned run = 0; run < 2; ++run) {
                crc = c;
                atomic_store(&now, 1000000);
                atomic_store(&stage, 0);
                atomic_store(&handshake, false);
                atomic_store(&gate, false);
                atomic_store(&entered, false);
                atomic_store(&triggered, false);
                atomic_store(&finished, false);
                atomic_store(&goodbyes, 0);
                atomic_store(&stopOrder, 0);
                atomic_store(&notified, 0);
                atomic_store(&saved, 0);
                atomic_store(&started[0], false);
                atomic_store(&started[1], false);
                session = IHS_TestSessionCreate();
                session->outgoing.rate[0] = session->outgoing.rate[1] = 0;
                static const IHS_StreamSessionCallbacks cb = {.disconnected = stopped,
                                                              .clientConfigChanged = changed,
                                                              .saveClientConfig = save};
                IHS_SessionSetSessionCallbacks(session, &cb, NULL);
                for (unsigned i = 0; i < 2; ++i) {
                    IHS_SessionChannel *d = IHS_SessionChannelDataCreate(
                        &dataClass, session,
                        i ? IHS_SessionChannelTypeDataVideo : IHS_SessionChannelTypeDataAudio,
                        3 + i, NULL);
                    assert(d);
                    IHS_SessionChannelAdd(session, d);
                }
                wait_bool(&started[0]);
                wait_bool(&started[1]);
                assert(IHS_SessionConnect(session));
                wait_bool(&entered);
                IHS_BaseLock(&session->base);
                session->state.connectionState = IHS_SessionConnectionStateConnected;
                session->state.crcRequired = crc;
                IHS_SessionChannelControl *control = (void *)session->channels[1];
                control->keepAliveTimestamp = control->frameEventsTimestamp = atomic_load(&now);
                IHS_BaseUnlock(&session->base);
                atomic_store(&gate, true);
                wait_bool(&finished);
                IHS_SessionThreadedJoin(session);
                assert(!session->base.worker && !session->sendThread &&
                       atomic_load(&goodbyes) == 1 && atomic_load(&stopOrder) == 3);
                assert(session->state.connectionState == IHS_SessionConnectionStateUnconnected);
                if (mode == 3) {
                    assert(!session->clientConfig && session->transport.detached);
                    assert(!((IHS_SessionChannelControl *)session->channels[1])->incomingCount);
                } else
                    assert(session->clientConfig->enable_unreliable_fec);
                IHS_SessionDestroy(session);
            }
    IHS_Quit();
    return 0;
}
