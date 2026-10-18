/* Actual RX and initial-send workers with an input batch deliberately held open.
 * Advancing the protocol clock must not let the sender perform periodic retries
 * before EOF. A later same-batch ACK cancels a NACK; without ACK, retry follows
 * EOF. Only final IO, readiness and the protocol clock are controlled. */
#include "ihs_buffer.h"
#include "ihs_buffer_ext.h"
#include "ihslib.h"
#include "session/session_pri.h"
#include "test_session.h"
#include <assert.h>
#include <stdatomic.h>
#include <unistd.h>

static IHS_Session *session;
static atomic_uint tick, stage, attempts, handshakeId;
static atomic_bool handshakeSent, gateOpen, gateEntered, eofSeen;
static bool crc, withAck, withTimeout;
static uint8_t originalLocal;
static atomic_uint goodbyes;
static atomic_bool closedNotice;
static unsigned ids[4], counts[4];
uint32_t __wrap_IHS_SessionPacketTimestamp(void) { return atomic_load(&tick); }
bool __wrap_IHS_BaseSend(IHS_Base *base, IHS_SocketAddress address, const IHS_Buffer *wire) {
    (void)base;
    (void)address;
    const uint8_t *p = IHS_BufferPointer(wire);
    if ((p[0] & 127) == 5 && p[4] == 1) {
        atomic_store(&handshakeId, p[7] | (unsigned)p[8] << 8);
        atomic_store(&handshakeSent, true);
    }
    if ((p[0] & 127) == 5 && p[4] == 4) {
        assert(wire->size == 14u + (crc ? 4u : 0u));
        assert((bool)(p[0] & 128) == crc && p[13] == 0xa5);
        unsigned n = atomic_load(&attempts);
        assert(n < 4);
        if (n)
            assert(atomic_load(&gateOpen) && atomic_load(&eofSeen) && !withAck);
        ids[n] = p[7] | (unsigned)p[8] << 8;
        counts[n] = p[1];
        atomic_fetch_add(&attempts, 1);
    }
    if (withTimeout && (p[0] & 127) == 9) {
        assert(atomic_load(&attempts) == 2 && atomic_load(&eofSeen));
        assert(IHS_SessionTransportState(session) == IHS_TransportConnected);
        assert(p[1] == 0 && p[2] == originalLocal && p[3] == 34 && p[4] == 0);
        assert((bool)(p[0] & 128) == crc);
        assert(atomic_fetch_add(&goodbyes, 1) == 0);
    }
    return true;
}
static void disconnected(IHS_Session *s, void *context) {
    (void)context;
    if (!withTimeout)
        return;
    assert(atomic_load(&goodbyes) == 1 && IHS_SessionTransportState(s) == IHS_TransportClosed);
    assert(s->state.connectionId != originalLocal && s->state.hostConnectionId == 0);
    atomic_store(&closedNotice, true);
}
static int incoming(IHS_UDPPacket *rx, unsigned type, unsigned channel, unsigned id,
                    bool extended) {
    IHS_SessionPacket p = {.header = {.type = type,
                                      .channelId = channel,
                                      .packetId = id,
                                      .srcConnectionId = 34,
                                      .dstConnectionId = session->state.connectionId,
                                      .hasCrc = type != 2 && crc,
                                      .sendTimestamp = 900000 + id}};
    IHS_SessionPacketBodyInitialize(&p.body, p.header.hasCrc);
    IHS_BufferAppendUInt32LE(&p.body, 1000000);
    if (extended)
        IHS_BufferAppendUInt16LE(&p.body, 65535);
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
        return incoming(rx, 2, 0, 0, false);
    case 1:
        atomic_store(&stage, 2);
        return 0;
    case 2:
        if (!atomic_load(&handshakeSent)) {
            usleep(1000);
            return 0;
        }
        atomic_store(&stage, 3);
        return incoming(rx, 7, 1, atomic_load(&handshakeId), false);
    case 3:
        atomic_store(&stage, 4);
        return 0;
    case 4:
        atomic_store(&gateEntered, true);
        while (!atomic_load(&gateOpen) && !session->base.interrupted)
            usleep(1000);
        if (withTimeout) {
            atomic_store(&stage, 7);
            atomic_store(&eofSeen, true);
            return 0;
        }
        atomic_store(&stage, 5);
        return incoming(rx, 8, 4, 1, true);
    case 5:
        atomic_store(&stage, 6);
        if (withAck)
            return incoming(rx, 7, 4, 0, false);
        /* fall through to the final empty read */
        /* FALLTHROUGH */
    case 6:
        atomic_store(&stage, 7);
        atomic_store(&eofSeen, true);
        return 0;
    default:
        usleep(1000);
        return 0;
    }
}
static void wait_flag(atomic_bool *flag) {
    uint64_t end = IHS_TimerNow() + 2000;
    while (!atomic_load(flag) && IHS_TimerNow() < end)
        usleep(1000);
    assert(atomic_load(flag));
}
int main(void) {
    IHS_Init();
    for (unsigned mode = 0; mode < 4; ++mode)
        for (unsigned c = 0; c < 2; ++c) {
            withAck = mode == 1;
            withTimeout = mode >= 2;
            crc = c;
            atomic_store(&tick, 1000000);
            atomic_store(&stage, 0);
            atomic_store(&attempts, 0);
            atomic_store(&handshakeSent, false);
            atomic_store(&gateOpen, false);
            atomic_store(&gateEntered, false);
            atomic_store(&eofSeen, false);
            atomic_store(&goodbyes, 0);
            atomic_store(&closedNotice, false);
            session = IHS_TestSessionCreate();
            session->outgoing.rate[0] = session->outgoing.rate[1] = 0;
            static const IHS_StreamSessionCallbacks cb = {.disconnected = disconnected};
            IHS_SessionSetSessionCallbacks(session, &cb, NULL);
            assert(IHS_SessionConnect(session));
            wait_flag(&gateEntered);
            /* Match the source fixture's live transport without enabling unrelated
             * Streaming control producers. Initial send remains independently live. */
            IHS_BaseLock(&session->base);
            session->state.crcRequired = crc;
            originalLocal = session->state.connectionId;
            if (withTimeout) {
                unsigned paused = mode == 3;
                session->transport.lastReceive = 1000100u - (paused ? 180u : 15u) * 65536u;
                session->transport.clientPaused = session->transport.connectionPaused = paused;
                session->transport.appliedGeneration = paused;
                atomic_store(&session->transport.lifecycleGeneration, paused);
            }
            IHS_OutgoingRegister(session, 4);
            IHS_BaseUnlock(&session->base);
            IHS_SessionPacket p = {.header = {.type = 5,
                                              .channelId = 4,
                                              .autoPacketId = true,
                                              .srcConnectionId = session->state.connectionId,
                                              .dstConnectionId = 34,
                                              .hasCrc = crc}};
            IHS_SessionPacketBodyInitialize(&p.body, crc);
            IHS_BufferAppendUInt8(&p.body, 0xa5);
            assert(IHS_SessionQueuePacket(session, &p, true));
            IHS_SessionPacketClear(&p, true);
            uint64_t end = IHS_TimerNow() + 2000;
            while (!atomic_load(&attempts) && IHS_TimerNow() < end)
                usleep(1000);
            assert(atomic_load(&attempts) == 1 && ids[0] == 0 && counts[0] == 0);
            atomic_store(&tick, 1000100);
            usleep(20000);
            assert(atomic_load(&attempts) == 1 && !atomic_load(&eofSeen));
            atomic_store(&gateOpen, true);
            wait_flag(&eofSeen);
            end = IHS_TimerNow() + 2000;
            if (!withAck) {
                while (atomic_load(&attempts) < 2 && IHS_TimerNow() < end)
                    usleep(1000);
                assert(atomic_load(&attempts) == 2 && ids[1] == 0 && counts[1] == 1);
            } else {
                while (IHS_RetransmissionIsTracked(&session->retransmission, 4, 0, 0) &&
                       IHS_TimerNow() < end)
                    usleep(1000);
                assert(!IHS_RetransmissionIsTracked(&session->retransmission, 4, 0, 0));
                usleep(20000);
                assert(atomic_load(&attempts) == 1);
            }
            if (withTimeout)
                wait_flag(&closedNotice);
            IHS_SessionInterrupt(session);
            IHS_SessionThreadedJoin(session);
            assert(!session->sendThread && !session->base.worker);
            if (withTimeout) {
                assert(atomic_load(&goodbyes) == 1);
                assert(IHS_SessionTransportState(session) == IHS_TransportClosed);
                assert(session->state.hostConnectionId == 0 &&
                       session->state.connectionId != originalLocal);
            }
            IHS_SessionDestroy(session);
        }
    IHS_Quit();
    return 0;
}
