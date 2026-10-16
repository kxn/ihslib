/* Actual receive/send worker launch, update, timeout callback and joins.
 * Only recvfrom input and stream clock are controlled; no external host. */
#include "ihs_buffer.h"
#include "ihs_buffer_ext.h"
#include "session/session_pri.h"
#include "test_session.h"
#include <assert.h>
#include <stdatomic.h>
#include <unistd.h>

static IHS_Session *session;
static atomic_uint tick;
static atomic_uint disconnected;
static bool background;
static bool receiveFailure;
static unsigned receives;
static const uint32_t start = 1000000;
uint32_t __wrap_IHS_SessionPacketTimestamp(void) {
    return atomic_load(&tick);
}
int __wrap_IHS_UDPSocketReceive(IHS_UDPSocket *socket, IHS_UDPPacket *received) {
    (void)socket;
    if (receives++ == 0) {
        IHS_SessionPacket packet = {.header = {.type = IHS_SessionPacketTypeConnectACK,
                                               .srcConnectionId = 34,
                                               .dstConnectionId = session->state.connectionId,
                                               .hasCrc = true,
                                               .sendTimestamp = start}};
        IHS_SessionPacketBodyInitialize(&packet.body, true);
        IHS_BufferAppendUInt32LE(&packet.body, start);
        IHS_SessionPacketPopulateBuffer(&packet);
        received->address = session->info.address;
        IHS_BufferAppendMem(&received->buffer, packet.body.data, IHS_SessionPacketSize(&packet));
        IHS_SessionPacketClear(&packet, true);
        if (background)
            IHS_SessionSetForeground(session, false);
        return 1;
    }
    if (receiveFailure)
        return -1;
    uint32_t threshold = (background ? 180u : 15u) * 65536u;
    if (receives == 2) {
        assert(session->state.connectionState == IHS_SessionConnectionStateHandshaking);
        assert(session->state.crcRequired);
        atomic_store(&tick, start + threshold - 1);
    } else if (receives == 3) {
        assert(!session->transport.timedOut);
        atomic_store(&tick, start + threshold);
    }
    usleep(1000);
    return 0;
}
static void on_disconnected(IHS_Session *s, void *context) {
    (void)s;
    (void)context;
    atomic_fetch_add(&disconnected, 1);
}
int main(void) {
    IHS_Init();
    for (unsigned i = 0; i < 3; ++i) {
        background = i == 1;
        receiveFailure = i == 2;
        receives = 0;
        atomic_store(&tick, start);
        atomic_store(&disconnected, 0);
        session = IHS_TestSessionCreate();
        assert(session);
        const IHS_StreamSessionCallbacks callbacks = {.disconnected = on_disconnected};
        IHS_SessionSetSessionCallbacks(session, &callbacks, NULL);
        assert(IHS_SessionConnect(session));
        IHS_SessionThreadedJoin(session);
        assert(session->transport.timedOut == !receiveFailure);
        if (!receiveFailure)
            assert(!session->transport.active);
        assert(session->base.interrupted);
        assert(!IHS_SessionHostRequestedStop(session));
        assert(atomic_load(&disconnected) == (receiveFailure ? 0u : 1u));
        IHS_SessionDestroy(session);
    }
    IHS_Quit();
    return 0;
}
