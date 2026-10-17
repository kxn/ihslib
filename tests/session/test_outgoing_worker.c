/* Real receive/send workers: queued budget rejection, local send failure,
 * retained packet -> raw NACK ingress -> retry, retirement and joined teardown.
 * Clock, recvfrom and final sendto are controlled; no Steam host is connected. */
#include "ihs_buffer.h"
#include "ihs_buffer_ext.h"
#include "session/session_pri.h"
#include "test_session.h"
#include <assert.h>
#include <errno.h>
#include <stdatomic.h>
#include <unistd.h>
static IHS_Session *session;
static atomic_uint tick, attempts, nackRequest;
static bool connected, failFirst;
static uint8_t counts[8];
static uint16_t ids[8];
uint32_t __wrap_IHS_SessionPacketTimestamp(void) {
    return atomic_load(&tick);
}
bool __wrap_IHS_BaseSend(IHS_Base *base, IHS_SocketAddress addr, const IHS_Buffer *wire) {
    (void)base;
    (void)addr;
    const uint8_t *p = IHS_BufferPointer(wire);
    if ((p[0] & 127) != 3 || p[4] != 7)
        return true;
    IHS_Buffer copy = IHS_BUFFER_INIT(2048, 2048);
    IHS_BufferAppendMem(&copy, p, wire->size);
    IHS_SessionPacket parsed;
    assert(IHS_SessionPacketParse(&parsed, &copy) == IHS_SessionPacketResultOK);
    assert(parsed.header.hasCrc && parsed.body.size == 1 &&
           *IHS_BufferPointer(&parsed.body) == 0xa5);
    unsigned index = atomic_load(&attempts);
    assert(index < 8);
    counts[index] = parsed.header.retransmitCount;
    ids[index] = parsed.header.packetId;
    IHS_SessionPacketClear(&parsed, true);
    IHS_BufferClear(&copy, true);
    atomic_fetch_add(&attempts, 1);
    if (failFirst) {
        failFirst = false;
        errno = EAGAIN;
        return false;
    }
    return true;
}
static int ScriptReceive(IHS_UDPSocket *sock, IHS_UDPPacket *received) {
    (void)sock;
    unsigned request = atomic_exchange(&nackRequest, 0);
    if (!connected || request) {
        IHS_SessionPacket p = {0};
        p.header.type = connected ? 10 : 2;
        p.header.srcConnectionId = 34;
        p.header.dstConnectionId = session->state.connectionId;
        p.header.channelId = connected ? 7 : 0;
        p.header.packetId = connected ? 1 : 0;
        p.header.hasCrc = true;
        p.header.sendTimestamp = atomic_load(&tick);
        IHS_SessionPacketBodyInitialize(&p.body, true);
        if (!connected)
            IHS_BufferAppendUInt32LE(&p.body, atomic_load(&tick));
        IHS_SessionPacketPopulateBuffer(&p);
        received->address = session->info.address;
        IHS_BufferAppendMem(&received->buffer, p.body.data, IHS_SessionPacketSize(&p));
        IHS_SessionPacketClear(&p, true);
        connected = true;
        return 1;
    }
    usleep(1000);
    return 0;
}
static void wait_attempts(unsigned n) {
    uint64_t end = IHS_TimerNow() + 3000;
    while (atomic_load(&attempts) < n && IHS_TimerNow() < end)
        usleep(1000);
    assert(atomic_load(&attempts) == n);
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
    for (unsigned launch = 0; launch < 2; ++launch) {
        connected = false;
        failFirst = true;
        atomic_store(&tick, 1000000);
        atomic_store(&attempts, 0);
        atomic_store(&nackRequest, 0);
        session = IHS_TestSessionCreate();
        assert(session);
        assert(IHS_SessionConnect(session));
        uint64_t end = IHS_TimerNow() + 3000;
        while (session->state.connectionState < IHS_SessionConnectionStateHandshaking &&
               IHS_TimerNow() < end)
            usleep(1000);
        assert(session->state.connectionState == IHS_SessionConnectionStateHandshaking);
        IHS_BaseLock(&session->base);
        session->state.connectionState = IHS_SessionConnectionStateConnected;
        session->outgoing.rate[0] = 65536;
        session->outgoing.deadline[0] = 1000010;
        IHS_BaseUnlock(&session->base);
        IHS_SessionChannel ch = {.session = session, .id = 7};
        IHS_SessionPacket p;
        IHS_SessionChannelInitializePacket(&ch, &p, 3, false, IHS_PACKET_ID_NEXT);
        IHS_BufferAppendUInt8(&p.body, 0xa5);
        assert(IHS_SessionQueuePacket(session, &p, false));
        IHS_SessionPacketClear(&p, true);
        bool cached = false;
        end = IHS_TimerNow() + 3000;
        while (!cached && IHS_TimerNow() < end) {
            IHS_BaseLock(&session->base);
            IHS_OutgoingWindow *w = &session->outgoing.unreliable[7];
            cached = w->capacity && w->slots[0] != NULL && session->outgoing.blocked;
            IHS_BaseUnlock(&session->base);
            usleep(1000);
        }
        assert(cached && !atomic_load(&attempts));
        atomic_store(&tick, 1000010);
        wait_attempts(2);
        assert(counts[0] == 0 && counts[1] == 0 && ids[0] == 0 && ids[1] == 0);
        atomic_store(&tick, 1000100);
        atomic_store(&nackRequest, 1);
        wait_attempts(3);
        assert(counts[2] == 1 && ids[2] == 0);
        atomic_store(&tick, 1020000);
        atomic_store(&nackRequest, 1);
        bool empty = false;
        end = IHS_TimerNow() + 3000;
        while (!empty && IHS_TimerNow() < end) {
            IHS_BaseLock(&session->base);
            IHS_OutgoingWindow *w = &session->outgoing.unreliable[7];
            empty = w->head == w->tail;
            IHS_BaseUnlock(&session->base);
            usleep(1000);
        }
        assert(empty && atomic_load(&attempts) == 3);
        IHS_SessionInterrupt(session);
        IHS_SessionThreadedJoin(session);
        assert(!session->sendThread && !session->base.worker);
        IHS_SessionDestroy(session);
    }
    IHS_Quit();
    return 0;
}
