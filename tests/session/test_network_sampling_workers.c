/* Concurrent real RX parse/admission, real packet-send preparation and live
 * snapshot reads. Final transport/time are controlled; threads join before
 * owner destruction. Official arithmetic/branches have separate vectors. */
#include "ihslib.h"
#include "session/session_pri.h"
#include "test_session.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>

static IHS_Session *session;
static atomic_uint finished, writes;
uint32_t __wrap_IHS_SessionPacketTimestamp(void) { return 1000000; }
bool __wrap_IHS_BaseSend(IHS_Base *base, IHS_SocketAddress address, const IHS_Buffer *wire) {
    (void)base; (void)address;
    assert(wire->size == 28 && (*IHS_BufferPointer(wire) & 128));
    atomic_fetch_add(&writes, 1);
    return true;
}
static void *send_packets(void *unused) {
    (void)unused;
    for (unsigned i = 0; i < 1000; ++i) {
        IHS_SessionPacket p = {.header = {.type = IHS_SessionPacketTypeReliable, .channelId = 1,
                                         .srcConnectionId = 12, .dstConnectionId = 34}};
        IHS_SessionPacketBodyInitialize(&p.body, false);
        IHS_BufferFillMem(&p.body, 0, 0, 11);
        assert(IHS_SessionSendPacketResult(session, &p) == 0);
        IHS_SessionPacketClear(&p, true);
    }
    atomic_fetch_add(&finished, 1);
    return NULL;
}
static void *receive_packets(void *unused) {
    (void)unused;
    for (unsigned i = 0; i < 1000; ++i) {
        /* Unknown admitted type is sampled before the connection ignores it. */
        IHS_SessionPacket p = {.header = {.type = 13, .channelId = 3, .hasCrc = true,
                                         .srcConnectionId = 34, .dstConnectionId = 12,
                                         .sendTimestamp = 1000000}};
        IHS_SessionPacketBodyInitialize(&p.body, true);
        IHS_BufferFillMem(&p.body, 0, 0, 7);
        IHS_SessionPacketPopulateBuffer(&p);
        IHS_Buffer wire = IHS_BUFFER_INIT(128, 128);
        IHS_BufferAppendMem(&wire, p.body.data, IHS_SessionPacketSize(&p));
        session->base.callbacks.received(&session->base, &session->info.address, &wire);
        IHS_BufferClear(&wire, true);
        IHS_SessionPacketClear(&p, true);
    }
    atomic_fetch_add(&finished, 1);
    return NULL;
}
static void *read_snapshots(void *unused) {
    (void)unused;
    do {
        float values[5];
        IHS_SessionGetNetworkSample(session, values);
        assert(values[0] == 0 && values[3] == 0 && values[4] == 0);
        assert(values[1] >= 0 && values[1] <= 54 && values[2] >= 0 && values[2] <= 64);
        sched_yield();
    } while (atomic_load(&finished) != 2);
    return NULL;
}
int main(void) {
    IHS_Init();
    session = IHS_TestSessionCreate();
    IHS_TimerTaskStopImmediate(session->retransmission.timer);
    session->state.connectionState = IHS_SessionConnectionStateConnected;
    session->state.connectionId = 12;
    session->state.hostConnectionId = 34;
    session->state.crcRequired = true;
    session->outgoing.rate[0] = session->outgoing.rate[1] = 0;
    atomic_init(&finished, 0);
    atomic_init(&writes, 0);
    pthread_t sender, receiver, reader;
    assert(!pthread_create(&sender, NULL, send_packets, NULL));
    assert(!pthread_create(&receiver, NULL, receive_packets, NULL));
    assert(!pthread_create(&reader, NULL, read_snapshots, NULL));
    assert(!pthread_join(sender, NULL) && !pthread_join(receiver, NULL) && !pthread_join(reader, NULL));
    assert(atomic_load(&writes) == 1000);
    assert(session->networkRates[0].bytes == 24000 && session->networkRates[1].bytes == 20000);
    assert(session->networkPackets.total == 1000 && session->networkPackets.lost == 0);
    assert(session->networkRates[0].queue.count == 1 && session->networkRates[1].queue.count == 1);
    assert(session->networkPackets.queue.count == 1);
    IHS_SessionDestroy(session);
    IHS_Quit();
    return 0;
}
