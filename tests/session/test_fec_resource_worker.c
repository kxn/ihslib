/* Actual Base receive/send and media workers, CRC connected exchange and FEC
 * OOM close/notification/join. Final UDP receive/send and allocation failure
 * are explicit adapters. Streaming is a controlled owner-thread precondition,
 * not an auth/negotiation/codec/Steam/Switch proof. */
#include "ihs_buffer.h"
#include "ihs_buffer_ext.h"
#include "ihslib.h"
#include "session/channels/ch_data.h"
#include "test_session.h"
#include <assert.h>
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>

static IHS_Session *session;
static _Thread_local unsigned fail_at;
static atomic_uint goodbyes, notices, media_starts, media_stops, failures, lost_calls;
static _Atomic(IHS_Thread *) media_worker;
static atomic_uint media_joins;
static unsigned receives, stage;
static bool end_batch, streaming;
void __real_IHS_ThreadJoin(IHS_Thread *);
void __wrap_IHS_ThreadJoin(IHS_Thread *thread) {
    bool media = thread && thread == atomic_load(&media_worker);
    __real_IHS_ThreadJoin(thread);
    if (media) {
        atomic_store(&media_worker, NULL);
        atomic_fetch_add(&media_joins, 1);
    }
}
void *__real_calloc(size_t, size_t);
void *__wrap_calloc(size_t n, size_t width) {
    if (fail_at && !--fail_at) {
        atomic_fetch_add(&failures, 1);
        return NULL;
    }
    return __real_calloc(n, width);
}
bool __real_IHS_FECReceiverAdd(IHS_FECReceiver *, const IHS_SessionPacket *, IHS_SessionFrame *);
bool __wrap_IHS_FECReceiverAdd(IHS_FECReceiver *r, const IHS_SessionPacket *p,
                               IHS_SessionFrame *f) {
    fail_at = stage + 1;
    bool ready = __real_IHS_FECReceiverAdd(r, p, f);
    assert(!ready && !fail_at && IHS_FECReceiverLastError(r) == IHS_FECReceiveNoMemory);
    return ready;
}
void __real_IHS_SessionChannelDataLost(IHS_SessionChannel *);
void __wrap_IHS_SessionChannelDataLost(IHS_SessionChannel *ch) {
    atomic_fetch_add(&lost_calls, 1);
    __real_IHS_SessionChannelDataLost(ch);
}
bool __wrap_IHS_BaseSend(IHS_Base *b, IHS_SocketAddress address, const IHS_Buffer *wire) {
    (void)b;
    (void)address;
    const uint8_t *bytes = IHS_BufferPointer(wire);
    unsigned type = bytes[0] & 127;
    if (type == IHS_SessionPacketTypeDisconnect) {
        assert(atomic_fetch_add(&goodbyes, 1) == 0);
        assert(wire->size == 17 && (bytes[0] & 128));
    } else {
        assert(!atomic_load(&goodbyes));
        assert(type != IHS_SessionPacketTypeACK && type != IHS_SessionPacketTypeNACK &&
               type != IHS_SessionPacketTypeUnreliableNack);
    }
    return true;
}
int __wrap_IHS_UDPSocketWaitReadable(IHS_UDPSocket *socket, uint32_t wait_ms) {
    (void)socket;
    (void)wait_ms;
    return 1;
}
int __wrap_IHS_UDPSocketReceive(IHS_UDPSocket *socket, IHS_UDPPacket *received) {
    (void)socket;
    if (end_batch) {
        end_batch = false;
        return 0;
    }
    unsigned step = receives++;
    assert(step < 2);
    IHS_SessionPacket p = {.header = {.type = step ? IHS_SessionPacketTypeUnreliableFEC
                                                   : IHS_SessionPacketTypeConnectACK,
                                      .srcConnectionId = 34,
                                      .dstConnectionId = session->state.connectionId,
                                      .hasCrc = true,
                                      .channelId = step ? 9 : 0,
                                      .fragmentId = step ? 1 : 0,
                                      .sendTimestamp = IHS_SessionPacketTimestamp()}};
    IHS_SessionPacketBodyInitialize(&p.body, true);
    if (!step) {
        IHS_BufferAppendUInt32LE(&p.body, p.header.sendTimestamp);
    } else {
        assert(IHS_SessionTransportState(session) == IHS_TransportConnected);
        if (streaming)
            session->state.connectionState = IHS_SessionConnectionStateConnected;
        IHS_BufferAppendUInt32LE(&p.body, 1);
        IHS_BufferAppendUInt16LE(&p.body, 32);
        IHS_BufferAppendUInt16LE(&p.body, 1);
        IHS_BufferAppendUInt8(&p.body, 'z');
        IHS_BufferFillMem(&p.body, p.body.size, 0, 31);
    }
    IHS_SessionPacketPopulateBuffer(&p);
    received->address = session->info.address;
    IHS_BufferAppendMem(&received->buffer, p.body.data, IHS_SessionPacketSize(&p));
    IHS_SessionPacketClear(&p, true);
    end_batch = true;
    return 1;
}
static void init(IHS_SessionChannel *ch, const void *unused) {
    (void)unused;
    assert(IHS_SessionChannelDataInit(ch, 2048));
}
static bool start(IHS_SessionChannel *ch) {
    (void)ch;
    atomic_fetch_add(&media_starts, 1);
    return true;
}
static void stop(IHS_SessionChannel *ch) {
    (void)ch;
    atomic_fetch_add(&media_stops, 1);
}
static void data(IHS_SessionChannel *ch, const IHS_SessionDataFrameHeader *h, IHS_Buffer *b) {
    (void)ch;
    (void)h;
    (void)b;
    assert(!"failed reconstruction must not reach a consumer");
}
static const IHS_SessionChannelDataClass cls = {
    .base = {.instanceSize = sizeof(IHS_SessionChannelData),
             .init = init,
             .deinit = IHS_SessionChannelDataDeinit,
             .stopped = IHS_SessionChannelDataStopped,
             .received = IHS_SessionChannelDataReceived},
    .start = start,
    .stop = stop,
    .dataFrame = data};
static void disconnected(IHS_Session *s, void *unused) {
    (void)s;
    (void)unused;
    assert(atomic_fetch_add(&notices, 1) == 0);
}
int main(void) {
    alarm(15);
    IHS_Init();
    for (unsigned mode = 0; mode < 8; ++mode) {
        stage = mode % 4;
        streaming = mode >= 4;
        receives = 0;
        end_batch = false;
        atomic_store(&goodbyes, 0);
        atomic_store(&notices, 0);
        atomic_store(&media_starts, 0);
        atomic_store(&media_stops, 0);
        atomic_store(&failures, 0);
        atomic_store(&lost_calls, 0);
        atomic_store(&media_joins, 0);
        session = IHS_TestSessionCreate();
        assert(session);
        IHS_TimerTaskStopImmediate(session->retransmission.timer);
        IHS_SessionChannel *ch =
            IHS_SessionChannelDataCreate(&cls, session, IHS_SessionChannelTypeDataVideo, 9, NULL);
        assert(ch);
        atomic_store(&media_worker, ((IHS_SessionChannelData *)ch)->worker);
        assert(atomic_load(&media_worker));
        IHS_SessionChannelAdd(session, ch);
        const IHS_StreamSessionCallbacks cb = {.disconnected = disconnected};
        IHS_SessionSetSessionCallbacks(session, &cb, NULL);
        assert(IHS_SessionConnect(session));
        IHS_SessionThreadedJoin(session);
        assert(receives == 2 && atomic_load(&failures) == 1 && atomic_load(&goodbyes) == 1 &&
               atomic_load(&notices) == 1 && !atomic_load(&lost_calls));
        assert(!session->sendThread && !session->base.worker);
        assert(atomic_load(&session->transport.closeRequested) && session->base.interrupted);
        assert(!IHS_SessionHostRequestedStop(session));
        IHS_SessionDestroy(session);
        assert(atomic_load(&media_starts) == 1 && atomic_load(&media_stops) == 1 &&
               atomic_load(&media_joins) == 1 && !atomic_load(&media_worker));
    }
    IHS_Quit();
    alarm(0);
    return 0;
}
