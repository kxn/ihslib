/* Actual UDP receive -> session admission -> channel Update -> Socket, then
 * stop/join. CRC x ACK/NACK failure x four errno classes use no timer override. */
#include "ihs_buffer.h"
#include "ihs_buffer_ext.h"
#include "session/channels/ch_control.h"
#include "session/session_pri.h"
#include "test_session.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>
static IHS_Session *session;
static atomic_uint now;
static atomic_bool handshake, done;
static unsigned phase, ackCount, nackCount, frames, failType, error, target;
static bool crc;
static pthread_t receiver;
uint32_t __wrap_IHS_SessionPacketTimestamp(void) { return atomic_load(&now); }
bool __real_IHS_SessionPacketsWindowPoll(IHS_SessionPacketsWindow *, IHS_SessionFrame *);
bool __wrap_IHS_SessionPacketsWindowPoll(IHS_SessionPacketsWindow *w, IHS_SessionFrame *f) {
    bool ready = __real_IHS_SessionPacketsWindowPoll(w, f);
    if (ready) {
        assert(pthread_equal(pthread_self(), receiver));
        ++frames;
    }
    return ready;
}
bool __wrap_IHS_BaseSend(IHS_Base *base, IHS_SocketAddress address, const IHS_Buffer *data) {
    (void)base;
    (void)address;
    const uint8_t *w = IHS_BufferPointer(data);
    unsigned type = w[0] & 127;
    if (type == 1 || type == 9)
        return true;
    assert(!!(w[0] & 128) == crc);
    if (type == 5) {
        atomic_store(&handshake, true);
        return true;
    }
    assert(w[4] == target && (type == 7 || type == 8));
    assert(pthread_equal(pthread_self(), receiver));
    unsigned id = w[7] | (unsigned)w[8] << 8;
    bool fail;
    if (type == 7) {
        const unsigned expected[] = {1, 2, 4, 4};
        assert(ackCount < 4 && id == expected[ackCount]);
        fail = failType == 7 && ackCount == 0;
        ++ackCount;
        assert(data->size == 17u + (crc ? 4u : 0u));
    } else {
        assert(id == 4 && nackCount < 2);
        assert(data->size == 20u + (crc ? 4u : 0u));
        assert(w[17] == 2 && w[18] == 0 && w[19] == 1);
        fail = failType == 8 && nackCount == 0;
        ++nackCount;
    }
    if (fail) {
        const int errors[] = {0, EAGAIN, EMSGSIZE, EBADF, EIO};
        errno = errors[error];
        return false;
    }
    return true;
}
static int incoming(IHS_UDPPacket *received, unsigned type, unsigned channel, unsigned id) {
    IHS_SessionPacket p = {.header = {.type = type,
                                      .channelId = channel,
                                      .srcConnectionId = 34,
                                      .dstConnectionId = session->state.connectionId,
                                      .packetId = id,
                                      .hasCrc = crc,
                                      .sendTimestamp = 900000 + id}};
    IHS_SessionPacketBodyInitialize(&p.body, crc);
    if (type == 5) {
        const uint8_t body[] = {6, 0xaa, 0xbb};
        IHS_BufferAppendMem(&p.body, body, sizeof(body));
    } else
        IHS_BufferAppendUInt32LE(&p.body, atomic_load(&now));
    IHS_SessionPacketPopulateBuffer(&p);
    received->address = session->info.address;
    IHS_BufferAppendMem(&received->buffer, p.body.data, IHS_SessionPacketSize(&p));
    IHS_SessionPacketClear(&p, true);
    return 1;
}
static int ScriptReceive(IHS_UDPSocket *socket, IHS_UDPPacket *received) {
    (void)socket;
    switch (phase) {
    case 0:
        receiver = pthread_self();
        ++phase;
        return incoming(received, 2, 0, 0);
    case 1:
        if (!atomic_load(&handshake))
            break;
        ++phase;
        return incoming(received, 7, 1, 0);
    case 2:
        ++phase;
        return incoming(received, 5, target, 0);
    case 3:
        assert(frames == 1 && ackCount == 0); /* initial watermark is already zero */
        ++phase;
        return incoming(received, 5, target, 1);
    case 4:
        assert(frames == 2 && ackCount == 1);
        ++phase;
        return incoming(received, 5, target, 2);
    case 5:
        assert(frames == 3 && ackCount == 1);
        ++phase;
        return 0;
    case 6:
        assert(ackCount == 1);
        ++phase;
        return 0;
    case 7:
        assert(ackCount == 2);
        ++phase;
        return incoming(received, 5, target, 4);
    case 8:
        assert(frames == 3 && nackCount == 0);
        ++phase;
        return 0;
    case 9:
        assert(nackCount == 0);
        ++phase;
        return 0;
    case 10:
        assert(nackCount == 1);
        ++phase;
        return 0;
    case 11:
        assert(nackCount == 1 && ackCount == 2); /* no automatic feedback retry */
        ++phase;
        return incoming(received, 5, target, 4);
    case 12:
        assert(nackCount == 2);
        ++phase;
        return incoming(received, 5, target, 3);
    case 13:
        assert(frames == 5 && ackCount == 3);
        ++phase;
        return incoming(received, 5, target, 0);
    case 14:
        assert(frames == 5 && ackCount == 4 && nackCount == 2);
        ++phase;
        atomic_store(&done, true);
        break;
    default:
        break;
    }
    usleep(1000);
    return 0;
}
static bool endBatch;
int __wrap_IHS_UDPSocketWaitReadable(IHS_UDPSocket *socket, uint32_t timeoutMs) {
    (void)socket; (void)timeoutMs;
    /* Clock advances before the read, matching Socket::Wait -> Update. */
    static const uint32_t times[] = {1000000,1000000,1000100,1000101,1000102,
        1000165,1000166,1000200,1000265,1000266,1000400,1000500,1000600,1000700};
    if (phase < sizeof(times)/sizeof(times[0])) atomic_store(&now, times[phase]);
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
    const unsigned targets[] = {0, 1, 2, 3, 31};
    for (unsigned t = 0; t < sizeof(targets) / sizeof(targets[0]); ++t)
        for (unsigned c = 0; c < 2; ++c)
            for (failType = 7; failType <= 8; ++failType)
                for (error = 1; error <= 4; ++error) {
                    target = targets[t];
                    crc = c != 0;
                    phase = ackCount = nackCount = frames = 0;
                    atomic_store(&now, 1000000);
                    atomic_store(&handshake, false);
                    atomic_store(&done, false);
                    session = IHS_TestSessionCreate();
                    assert(IHS_SessionConnect(session));
                    for (unsigned i = 0; !atomic_load(&done) && i < 1000; ++i)
                        usleep(1000);
                    assert(atomic_load(&done));
                    IHS_SessionDisconnect(session);
                    IHS_SessionThreadedJoin(session);
                    assert(!session->sendThread && !session->base.worker);
                    assert(!session->transport.timedOut && ackCount == 4 && nackCount == 2);
                    IHS_SessionDestroy(session);
                }
    IHS_Quit();
    return 0;
}
