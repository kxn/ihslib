/* Actual startup, initial-send/RTO failures, ACK retirement and joins. Clock and
 * final UDP only are controlled. Failed Socket writes must not end transport. */
#include "ihs_buffer.h"
#include "ihs_buffer_ext.h"
#include "session/channels/ch_control.h"
#include "session/session_pri.h"
#include "test_session.h"
#include <assert.h>
#include <errno.h>
#include <stdatomic.h>
#include <unistd.h>
static IHS_Session *session;
static atomic_uint tick, attempts, stamp;
static atomic_bool delivered, done;
static unsigned receives;
static bool ackSent;
uint32_t __wrap_IHS_SessionPacketTimestamp(void) {
    return atomic_load(&tick);
}
bool __wrap_IHS_BaseSend(IHS_Base *base, IHS_SocketAddress address, const IHS_Buffer *data) {
    (void)base;
    (void)address;
    const uint8_t *wire = IHS_BufferPointer(data);
    unsigned type = wire[0] & 127;
    if (type == IHS_SessionPacketTypeConnect)
        return true;
    if (type == IHS_SessionPacketTypeDisconnect) {
        assert(atomic_load(&done));
        return true;
    }
    assert(type == IHS_SessionPacketTypeReliable && wire[4] == 1);
    assert(wire[7] == 0 && wire[8] == 0); /* pre-connect rejection consumed no ID */
    assert(wire[1] == 0);                 /* failed attempts did not increase the wire count */
    assert(wire[13] == k_EStreamControlClientHandshake);
    const int errors[] = {EAGAIN, EIO, EMSGSIZE, EBADF};
    unsigned attempt = atomic_fetch_add(&attempts, 1);
    if (attempt < 4) {
        errno = errors[attempt];
        return false;
    }
    uint32_t time =
        wire[9] | (uint32_t)wire[10] << 8 | (uint32_t)wire[11] << 16 | (uint32_t)wire[12] << 24;
    atomic_store(&stamp, time);
    atomic_store(&delivered, true);
    return true;
}
static int incoming(IHS_UDPPacket *received, unsigned type) {
    IHS_SessionPacket p = {.header = {.type = type,
                                      .channelId = type == IHS_SessionPacketTypeACK ? 1 : 0,
                                      .srcConnectionId = 34,
                                      .dstConnectionId = session->state.connectionId,
                                      .hasCrc = true,
                                      .sendTimestamp = atomic_load(&tick)}};
    IHS_SessionPacketBodyInitialize(&p.body, true);
    IHS_BufferAppendUInt32LE(&p.body,
                             type == IHS_SessionPacketTypeACK ? atomic_load(&stamp) : 1000000);
    IHS_SessionPacketPopulateBuffer(&p);
    received->address = session->info.address;
    IHS_BufferAppendMem(&received->buffer, p.body.data, IHS_SessionPacketSize(&p));
    IHS_SessionPacketClear(&p, true);
    return 1;
}
static int ScriptReceive(IHS_UDPSocket *socket, IHS_UDPPacket *received) {
    (void)socket;
    if (receives++ == 0)
        return incoming(received, IHS_SessionPacketTypeConnectACK);
    if (!atomic_load(&done))
        assert(IHS_SessionTransportState(session) == IHS_TransportConnected);
    if (atomic_load(&delivered) && !ackSent) {
        ackSent = true;
        return incoming(received, IHS_SessionPacketTypeACK);
    }
    if (ackSent) {
        IHS_RetransmissionStats stats;
        IHS_RetransmissionGetStats(&session->retransmission, &stats, IHS_TimerNow());
        if (!stats.outstanding)
            atomic_store(&done, true);
    }
    /* Bigger than the largest ordinary RTO; still below the 15s lease. */
    if (!atomic_load(&done))
        atomic_fetch_add(&tick, 5000);
    assert(receives < 150);
    usleep(1000);
    return 0;
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
    for (unsigned run = 0; run < 2; ++run) {
        receives = 0;
        ackSent = false;
        atomic_store(&tick, 1000000);
        atomic_store(&attempts, 0);
        atomic_store(&delivered, false);
        atomic_store(&done, false);
        session = IHS_TestSessionCreate();
        CStreamingClientHandshakeInfo info = CSTREAMING_CLIENT_HANDSHAKE_INFO__INIT;
        CClientHandshakeMsg message = CCLIENT_HANDSHAKE_MSG__INIT;
        message.info = &info;
        assert(!IHS_SessionSendControlMessage(session, k_EStreamControlClientHandshake,
                                              &message.base));
        assert(!session->base.interrupted && session->outgoing.reliableId[1] == 0);
        assert(IHS_SessionConnect(session));
        for (unsigned wait = 0; !atomic_load(&done) && wait < 1000; ++wait)
            usleep(1000);
        assert(atomic_load(&done) && atomic_load(&attempts) == 5);
        assert(!session->base.interrupted && !session->transport.timedOut);
        IHS_SessionDisconnect(session);
        IHS_SessionThreadedJoin(session);
        assert(!session->sendThread && !session->base.worker);
        IHS_SessionDestroy(session);
    }
    IHS_Quit();
    return 0;
}
