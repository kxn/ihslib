/* Full workers, concurrent application close, peer close, timeout and early
 * cancellation. Final UDP I/O and time are controlled; no host/device. */
#include "ihs_buffer.h"
#include "ihs_buffer_ext.h"
#include "session/session_pri.h"
#include "test_session.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>

enum { Local, Peer, Timeout, Early };
static IHS_Session *session;
static atomic_uint tick, goodbyes, notices;
static atomic_bool ready;
static unsigned mode, receives;
static bool crc;
static const uint32_t start = 1000000;
uint32_t __wrap_IHS_SessionPacketTimestamp(void) {
    return atomic_load(&tick);
}
bool __wrap_IHS_BaseSend(IHS_Base *b, IHS_SocketAddress addr, const IHS_Buffer *wire) {
    (void)b;
    (void)addr;
    unsigned type = IHS_BufferPointer(wire)[0] & 127;
    if (type != IHS_SessionPacketTypeDisconnect) {
        assert(atomic_load(&goodbyes) == 0); /* no data after the goodbye attempt */
        return true;
    }
    assert(atomic_fetch_add(&goodbyes, 1) == 0);
    assert(mode != Early && wire->size == (crc ? 17u : 13u));
    IHS_Buffer data = IHS_BUFFER_INIT(64, 64);
    IHS_BufferAppendMem(&data, IHS_BufferPointer(wire), wire->size);
    IHS_SessionPacket packet;
    assert(IHS_SessionPacketParse(&packet, &data) == IHS_SessionPacketResultOK);
    assert(packet.header.hasCrc == crc && packet.header.retransmitCount == 0);
    assert(packet.header.srcConnectionId == session->state.connectionId);
    assert(packet.header.dstConnectionId == 34 && packet.header.channelId == 0);
    assert(packet.header.packetId == 0 && packet.header.fragmentId == 0 && packet.body.size == 0);
    IHS_SessionPacketClear(&packet, true);
    IHS_BufferClear(&data, true);
    errno = EAGAIN; /* failure is deliberately not retried */
    return false;
}
static int incoming(IHS_UDPPacket *received, unsigned type, bool hasCrc) {
    IHS_SessionPacket packet = {.header = {.type = type,
                                           .srcConnectionId = 34,
                                           .dstConnectionId = session->state.connectionId,
                                           .hasCrc = hasCrc,
                                           .sendTimestamp = start}};
    IHS_SessionPacketBodyInitialize(&packet.body, hasCrc);
    if (type == IHS_SessionPacketTypeConnectACK)
        IHS_BufferAppendUInt32LE(&packet.body, start);
    IHS_SessionPacketPopulateBuffer(&packet);
    received->address = session->info.address;
    IHS_BufferAppendMem(&received->buffer, packet.body.data, IHS_SessionPacketSize(&packet));
    IHS_SessionPacketClear(&packet, true);
    return 1;
}
static int ScriptReceive(IHS_UDPSocket *socket, IHS_UDPPacket *received) {
    (void)socket;
    if (mode == Early) {
        usleep(1000);
        return 0;
    }
    if (receives++ == 0)
        return incoming(received, IHS_SessionPacketTypeConnectACK, crc);
    atomic_store(&ready, true);
    if (receives == 2) {
        assert(IHS_SessionTransportState(session) == IHS_TransportConnected);
        if (mode == Peer)
            return incoming(received, IHS_SessionPacketTypeDisconnect, false);
        if (mode == Timeout)
            atomic_store(&tick, start + 15u * 65536u);
    }
    usleep(1000);
    return 0;
}
static void disconnected(IHS_Session *s, void *ctx) {
    (void)ctx;
    assert(atomic_fetch_add(&notices, 1) == 0);
    if (mode == Peer || mode == Timeout) {
        assert(!s->transport.detached);
        assert(IHS_SessionTransportState(s) == IHS_TransportClosing);
        assert(s->state.hostConnectionId == 34);
    }
}
static void *close_session(void *ctx) {
    (void)ctx;
    IHS_SessionDisconnect(session);
    return NULL;
}
static void *produce(void *ctx) {
    (void)ctx;
    CKeepAliveMsg message = CKEEP_ALIVE_MSG__INIT;
    for (unsigned i = 0; i < 64; ++i)
        IHS_SessionSendControlMessage(session, k_EStreamControlKeepAlive,
                                      (const ProtobufCMessage *)&message);
    return NULL;
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
    for (unsigned run = 0; run < 12; ++run) {
        mode = run % 3;
        crc = run & 1;
        receives = 0;
        atomic_store(&tick, start);
        atomic_store(&goodbyes, 0);
        atomic_store(&notices, 0);
        atomic_store(&ready, false);
        session = IHS_TestSessionCreate();
        const IHS_StreamSessionCallbacks cb = {.disconnected = disconnected};
        IHS_SessionSetSessionCallbacks(session, &cb, NULL);
        assert(IHS_SessionConnect(session));
        if (mode == Local) {
            for (unsigned wait = 0; !atomic_load(&ready) && wait < 1000; ++wait)
                usleep(1000);
            assert(atomic_load(&ready));
            pthread_t threads[9];
            assert(!pthread_create(&threads[0], NULL, produce, NULL));
            for (unsigned i = 1; i < 9; ++i)
                assert(!pthread_create(&threads[i], NULL, close_session, NULL));
            for (unsigned i = 0; i < 9; ++i)
                assert(!pthread_join(threads[i], NULL));
        }
        IHS_SessionThreadedJoin(session);
        assert(atomic_load(&goodbyes) == 1 && atomic_load(&notices) == 1);
        assert(session->transport.detached &&
               IHS_SessionTransportState(session) == IHS_TransportClosed);
        assert(session->transport.timedOut == (mode == Timeout));
        assert(IHS_SessionHostRequestedStop(session) == (mode == Peer));
        assert(!session->sendThread && !session->base.worker);
        IHS_SessionDisconnect(session); /* no new goodbye or notification */
        assert(atomic_load(&goodbyes) == 1 && atomic_load(&notices) == 1);
        IHS_SessionDestroy(session);
    }
    mode = Early;
    for (unsigned run = 0; run < 32; ++run) {
        atomic_store(&goodbyes, 0);
        atomic_store(&notices, 0);
        session = IHS_TestSessionCreate();
        const IHS_StreamSessionCallbacks cb = {.disconnected = disconnected};
        IHS_SessionSetSessionCallbacks(session, &cb, NULL);
        assert(IHS_SessionConnect(session));
        IHS_SessionDisconnect(session);
        IHS_SessionThreadedJoin(session);
        assert(!atomic_load(&goodbyes) && atomic_load(&notices) == 1);
        assert(session->transport.detached && !session->sendThread && !session->base.worker);
        IHS_SessionDestroy(session);
    }
    IHS_Quit();
    return 0;
}
