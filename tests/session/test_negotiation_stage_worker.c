/* Real RX/send workers: peer config, Complete, exact KeepAlive tick and IO
 * failure, then stop/join. Authentication/host/clock/final UDP are controlled. */
#include "ihs_buffer.h"
#include "ihs_buffer_ext.h"
#include "session/channels/ch_control.h"
#include "session/session_pri.h"
#include "test_session.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <unistd.h>
static IHS_Session *s;
static atomic_uint now, completeId, keepaliveId;
static atomic_bool handshake, complete, keepalive, failed, connected, done;
static atomic_uint phase;
static unsigned fault;
static bool crc;
static uint32_t start;
static pthread_t receiver;
uint32_t __wrap_IHS_SessionPacketTimestamp(void) {
    return atomic_load(&now);
}
bool __wrap_IHS_BaseSend(IHS_Base *b, IHS_SocketAddress address, const IHS_Buffer *data) {
    (void)b;
    (void)address;
    const uint8_t *w = IHS_BufferPointer(data);
    unsigned type = w[0] & 127;
    if (type == 1 || type == 7 || type == 8 || type == 9)
        return true;
    assert(type == 5 && w[4] == 1);
    unsigned tag = w[13];
    if (tag == 6) {
        atomic_store(&handshake, true);
        return true;
    }
    assert(tag == 5 || tag == 9);
    assert(!!(w[0] & 128) == crc);
    if (((fault == 1 && tag == 5) || (fault >= 2 && tag == 9)) && !atomic_exchange(&failed, true)) {
        errno = fault == 3 ? EAGAIN : EIO;
        return false;
    }
    IHS_Buffer cipher = IHS_BUFFER_WRAP((uint8_t *)w + 14, data->size - 14 - (crc ? 4 : 0));
    IHS_Buffer plain = IHS_BUFFER_INIT(1024, 0);
    uint64_t sequence;
    assert(!IHS_SessionFrameDecrypt(s, &cipher, &plain, tag == 5 ? 0 : 1, &sequence));
    assert(!plain.size);
    IHS_BufferClear(&plain, true);
    unsigned id = w[7] | (unsigned)w[8] << 8;
    if (tag == 5) {
        atomic_store(&completeId, id);
        atomic_store(&complete, true);
    } else {
        assert((uint32_t)(atomic_load(&now) - (start + 0xa0001u)) < 65536u);
        atomic_store(&keepaliveId, id);
        atomic_store(&keepalive, true);
    }
    return true;
}
static int incoming(IHS_UDPPacket *rx, unsigned type, unsigned id) {
    IHS_SessionPacket p = {.header = {.type = type,
                                      .channelId = type == 2 ? 0 : 1,
                                      .srcConnectionId = 34,
                                      .dstConnectionId = s->state.connectionId,
                                      .packetId = id,
                                      .hasCrc = type != 2 && crc,
                                      .sendTimestamp = atomic_load(&now)}};
    IHS_SessionPacketBodyInitialize(&p.body, p.header.hasCrc);
    if (type == 5) {
        static const uint8_t config[] = {0x0a, 6, 8, 0, 0x10, 3, 0x18, 4};
        uint8_t cipher[128];
        size_t size = sizeof(cipher);
        assert(!IHS_SessionFrameEncrypt(s, config, sizeof(config), cipher, &size, 0));
        IHS_BufferAppendUInt8(&p.body, 4);
        IHS_BufferAppendMem(&p.body, cipher, size);
    } else
        IHS_BufferAppendUInt32LE(&p.body, atomic_load(&now));
    IHS_SessionPacketPopulateBuffer(&p);
    rx->address = s->info.address;
    IHS_BufferAppendMem(&rx->buffer, p.body.data, IHS_SessionPacketSize(&p));
    IHS_SessionPacketClear(&p, true);
    return 1;
}
static int ScriptReceive(IHS_UDPSocket *socket, IHS_UDPPacket *rx) {
    (void)socket;
    switch (phase) {
    case 0:
        receiver = pthread_self();
        ++phase;
        return incoming(rx, 2, 0);
    case 1:
        if (!atomic_load(&handshake))
            break;
        ++phase;
        return incoming(rx, 7, 0);
    case 2:
        /* Post-auth fixture: the rest uses actual encrypted control ingress. */
        s->state.connectionState = IHS_SessionConnectionStateNegotiating;
        s->state.crcRequired = crc;
        ++phase;
        return incoming(rx, 5, 0);
    case 3:
        if (!atomic_load(&complete))
            break;
        assert(atomic_load(&connected));
        assert(s->negotiatedConfig && !s->negotiatedConfig->reliable_data);
        assert(s->negotiatedConfig->selected_audio_codec == 3);
        assert(s->negotiatedConfig->selected_video_codec == 4);
        assert(((IHS_SessionChannelControl *)s->channels[1])->keepAliveTimestamp == start);
        ++phase;
        return incoming(rx, 7, atomic_load(&completeId));
    case 4:
        atomic_store(&now, start + 0xa0000u);
        ++phase;
        return 0;
    case 5:
        assert(!atomic_load(&keepalive));
        assert(((IHS_SessionChannelControl *)s->channels[1])->sendEncryptSequence == 1);
        atomic_store(&now, start + 0xa0001u);
        ++phase;
        return 0;
    case 6:
        if (!atomic_load(&keepalive)) {
            if (fault == 3 && atomic_load(&failed))
                atomic_store(&now, start + 0xa0002u + IHS_StreamClockRetryTicks(&s->clock));
            break;
        }
        assert(((IHS_SessionChannelControl *)s->channels[1])->sendEncryptSequence == 2);
        assert(((IHS_SessionChannelControl *)s->channels[1])->keepAliveTimestamp ==
               start + 0xa0001u);
        ++phase;
        return incoming(rx, 7, atomic_load(&keepaliveId));
    case 7:
        atomic_store(&done, true);
        break;
    }
    usleep(500);
    return 0;
}
static void onConnected(IHS_Session *session, void *ctx) {
    (void)session;
    (void)ctx;
    assert(pthread_equal(pthread_self(), receiver));
    assert(!atomic_exchange(&connected, true));
    assert(s->state.connectionState == IHS_SessionConnectionStateConnected);
}
static bool endBatch;
int __wrap_IHS_UDPSocketWaitReadable(IHS_UDPSocket *socket, uint32_t timeoutMs) {
    (void)socket; (void)timeoutMs;
    return 1;
}
int __wrap_IHS_UDPSocketReceive(IHS_UDPSocket *socket, IHS_UDPPacket *packet) {
    if (endBatch) { endBatch = false; return 0; }
    int result = ScriptReceive(socket, packet);
    endBatch = result > 0;
    return result;
}

int main(void) {
    IHS_Init();
    for (unsigned c = 0; c < 2; ++c)
        for (unsigned wrap = 0; wrap < 2; ++wrap)
            for (fault = 0; fault < 4; ++fault) {
                crc = c;
                start = wrap ? 0xffff0000u : 1000000u;
                atomic_store(&now, start);
                atomic_store(&handshake, false);
                atomic_store(&complete, false);
                atomic_store(&keepalive, false);
                atomic_store(&failed, false);
                atomic_store(&connected, false);
                atomic_store(&done, false);
                phase = 0;
                s = IHS_TestSessionCreate();
                s->outgoing.rate[0] = s->outgoing.rate[1] = 0;
                const IHS_StreamSessionCallbacks callbacks = {.connected = onConnected};
                IHS_SessionSetSessionCallbacks(s, &callbacks, NULL);
                assert(IHS_SessionConnect(s));
                uint64_t end = IHS_TimerNow() + 5000;
                while (!atomic_load(&done) && IHS_TimerNow() < end)
                    usleep(1000);
                if (!atomic_load(&done))
                    fprintf(stderr,
                            "negotiation worker c=%u wrap=%u fault=%u phase=%u handshake=%u "
                            "complete=%u connected=%u keepalive=%u failed=%u\n",
                            c, wrap, fault, atomic_load(&phase), atomic_load(&handshake),
                            atomic_load(&complete), atomic_load(&connected),
                            atomic_load(&keepalive), atomic_load(&failed));
                assert(atomic_load(&done) && atomic_load(&connected));
                assert(atomic_load(&failed) == (fault != 0));
                IHS_SessionDisconnect(s);
                IHS_SessionThreadedJoin(s);
                IHS_SessionDestroy(s);
            }
    IHS_Quit();
    return 0;
}
