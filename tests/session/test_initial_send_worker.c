/* Actual connect -> reliable three-fragment failure/recovery -> ACK -> joins.
 * All 24 CRC/failure-position/errno combinations use only clock/final-I/O doubles. */
#include "ihs_buffer.h"
#include "ihs_buffer_ext.h"
#include "session/channels/ch_stats.h"
#include "session/session_pri.h"
#include "test_session.h"
#include <assert.h>
#include <errno.h>
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>
static IHS_Session *session;
static atomic_bool handshake, delivered, done;
static atomic_uint attempts;
static bool crc, handshakeAck, statsAck;
static unsigned receives, failIndex, code, ids[8];
uint32_t __wrap_IHS_SessionPacketTimestamp(void) {
    return 1000000;
}
bool __wrap_IHS_BaseSend(IHS_Base *base, IHS_SocketAddress address, const IHS_Buffer *data) {
    (void)base;
    (void)address;
    const uint8_t *wire = IHS_BufferPointer(data);
    unsigned type = wire[0] & 127;
    if (type == 1 || type == 9)
        return true;
    assert(!!(wire[0] & 128) == crc && wire[1] == 0);
    if (wire[4] == 1) {
        assert(type == 5 && wire[13] == k_EStreamControlClientHandshake);
        atomic_store(&handshake, true);
        return true;
    }
    assert(wire[4] == 2 && (type == 5 || type == 6));
    unsigned n = atomic_fetch_add(&attempts, 1);
    assert(n < 8);
    ids[n] = wire[7] | (unsigned)wire[8] << 8;
    if (n == failIndex) {
        const int errors[] = {0, EAGAIN, EMSGSIZE, EBADF, EIO};
        errno = errors[code];
        return false;
    }
    if (ids[n] == 2)
        atomic_store(&delivered, true);
    return true;
}
static int incoming(IHS_UDPPacket *received, unsigned type, unsigned channel, unsigned id) {
    IHS_SessionPacket p = {.header = {.type = type,
                                      .channelId = channel,
                                      .srcConnectionId = 34,
                                      .dstConnectionId = session->state.connectionId,
                                      .packetId = id,
                                      .hasCrc = crc,
                                      .sendTimestamp = 1000000}};
    IHS_SessionPacketBodyInitialize(&p.body, crc);
    IHS_BufferAppendUInt32LE(&p.body, 1000000);
    IHS_SessionPacketPopulateBuffer(&p);
    received->address = session->info.address;
    IHS_BufferAppendMem(&received->buffer, p.body.data, IHS_SessionPacketSize(&p));
    IHS_SessionPacketClear(&p, true);
    return 1;
}
static int ScriptReceive(IHS_UDPSocket *socket, IHS_UDPPacket *received) {
    (void)socket;
    if (receives++ == 0)
        return incoming(received, 2, 0, 0);
    if (atomic_load(&handshake) && !handshakeAck) {
        handshakeAck = true;
        return incoming(received, 7, 1, 0);
    }
    if (atomic_load(&delivered) && !statsAck) {
        statsAck = true;
        return incoming(received, 7, 2, 2);
    }
    if (statsAck) {
        IHS_RetransmissionStats stats;
        IHS_RetransmissionGetStats(&session->retransmission, &stats, IHS_TimerNow());
        if (!stats.outstanding)
            atomic_store(&done, true);
    }
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
    for (unsigned c = 0; c < 2; ++c)
        for (failIndex = 0; failIndex < 3; ++failIndex)
            for (code = 1; code <= 4; ++code) {
                crc = c != 0;
                receives = 0;
                handshakeAck = statsAck = false;
                atomic_store(&handshake, false);
                atomic_store(&delivered, false);
                atomic_store(&done, false);
                atomic_store(&attempts, 0);
                session = IHS_TestSessionCreate();
                assert(IHS_SessionConnect(session));
                for (unsigned i = 0; !atomic_load(&handshake) && i < 1000; ++i)
                    usleep(1000);
                assert(atomic_load(&handshake));
                IHS_BaseLock(&session->base);
                session->state.mtu = 512;
                session->outgoing.rate[0] = session->outgoing.rate[1] = 0;
                session->outgoing.deadline[0] = session->outgoing.deadline[1] = 1000000;
                IHS_BaseUnlock(&session->base);
                char text[1001];
                memset(text, 'x', 1000);
                text[1000] = 0;
                CLogMsg message = CLOG_MSG__INIT;
                message.message = text;
                assert(IHS_SessionChannelStatsSend(session->channels[2], k_EStreamStatsLogMessage,
                                                   &message.base, IHS_PACKET_ID_NEXT));
                for (unsigned i = 0; !atomic_load(&done) && i < 1000; ++i)
                    usleep(1000);
                assert(atomic_load(&done));
                IHS_SessionDisconnect(session);
                IHS_SessionThreadedJoin(session);
                assert(!session->sendThread && !session->base.worker);
                assert(atomic_load(&attempts) == 4);
                unsigned n = 0;
                for (unsigned i = 0; i <= failIndex; ++i)
                    assert(ids[n++] == i);
                for (unsigned i = failIndex; i < 3; ++i)
                    assert(ids[n++] == i);
                assert(n == 4 && !session->transport.timedOut);
                IHS_SessionDestroy(session);
            }
    IHS_Quit();
    return 0;
}
