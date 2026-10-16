/* Actual initialized/update/receive/send/finalized workers; final I/O and clock
 * only are controlled. No host. Run twice to exercise joins and fresh state. */
#include "ihs_buffer.h"
#include "ihs_buffer_ext.h"
#include "session/session_pri.h"
#include "test_session.h"
#include <assert.h>
#include <errno.h>
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>

static IHS_Session *session;
static atomic_uint tick, attempts;
static unsigned receives;
static const uint32_t start = 1000000;
uint32_t __wrap_IHS_SessionPacketTimestamp(void) { return atomic_load(&tick); }
bool __wrap_IHS_BaseSend(IHS_Base *base, IHS_SocketAddress addr, const IHS_Buffer *data) {
    (void)base;
    (void)addr;
    const uint8_t *wire = IHS_BufferPointer(data);
    if ((wire[0] & 127) != 1)
        return true;
    assert(data->size == 17 && wire[0] == 1 && wire[1] == 0);
    assert(wire[2] == session->state.connectionId);
    static const uint8_t zero[6] = {0};
    static const uint8_t cookie[] = {0xc7, 0x3d, 0x8f, 0x3c};
    assert(!memcmp(wire + 3, zero, sizeof(zero)));
    assert(!memcmp(wire + 13, cookie, sizeof(cookie)));
    unsigned index = atomic_fetch_add(&attempts, 1);
    errno = index ? EAGAIN : EIO;
    return index >= 2;
}
static int connected(IHS_UDPPacket *received, unsigned size) {
    IHS_SessionPacket packet = {.header = {.type = IHS_SessionPacketTypeConnectACK,
        .srcConnectionId = 34, .dstConnectionId = session->state.connectionId,
        .hasCrc = true, .sendTimestamp = atomic_load(&tick)}};
    IHS_SessionPacketBodyInitialize(&packet.body, true);
    uint8_t body[4] = {0};
    assert(size <= sizeof(body));
    IHS_BufferAppendMem(&packet.body, body, size);
    IHS_SessionPacketPopulateBuffer(&packet);
    received->address = session->info.address;
    IHS_BufferAppendMem(&received->buffer, packet.body.data, IHS_SessionPacketSize(&packet));
    IHS_SessionPacketClear(&packet, true);
    return 1;
}
int __wrap_IHS_UDPSocketReceive(IHS_UDPSocket *socket, IHS_UDPPacket *received) {
    (void)socket;
    switch (receives++) {
    case 0:
        assert(atomic_load(&attempts) == 2); /* initial failure + immediate Update failure */
        atomic_store(&tick, start + 6552);
        return 0;
    case 1:
        assert(atomic_load(&attempts) == 2);
        atomic_store(&tick, start + 6553);
        return 0;
    case 2:
        assert(atomic_load(&attempts) == 3);
        return connected(received, 3); /* valid CRC; body guard must reject */
    case 3:
        assert(session->state.connectionState == IHS_SessionConnectionStateConnecting);
        atomic_store(&tick, start + 13106);
        return 0;
    case 4:
        assert(atomic_load(&attempts) == 4);
        return connected(received, 4);
    case 5:
        assert(session->state.connectionState == IHS_SessionConnectionStateHandshaking);
        assert(session->state.crcRequired);
        atomic_store(&tick, start + 19659);
        return 0;
    default:
        assert(atomic_load(&attempts) == 4); /* accepted Connected stops retries */
        usleep(1000);
        return -1;
    }
}
int main(void) {
    IHS_Init();
    for (unsigned run = 0; run < 2; ++run) {
        atomic_store(&tick, start);
        atomic_store(&attempts, 0);
        receives = 0;
        session = IHS_TestSessionCreate();
        assert(IHS_SessionConnect(session));
        IHS_SessionThreadedJoin(session);
        assert(session->base.interrupted && !session->transport.timedOut);
        assert(!IHS_SessionHostRequestedStop(session));
        assert(atomic_load(&attempts) == 4);
        assert(session->sendThread == NULL && session->base.worker == NULL);
        IHS_SessionDestroy(session);
    }
    IHS_Quit();
    return 0;
}
