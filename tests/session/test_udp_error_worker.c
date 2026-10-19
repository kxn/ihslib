/* Actual session Base RX/send and media worker. Wait/read failure adapters are
 * nonterminal; a later CRC handshake and maximum receive-size padded FEC frame
 * reach the consumer. No Steam authentication, codec or Switch claim. */
#include "crc32c.h"
#include "ihs_buffer.h"
#include "ihs_buffer_ext.h"
#include "session/channels/ch_data.h"
#include "test_session.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static IHS_Session *session;
static bool wait_failure, crc;
static unsigned waits, reads;
static atomic_uint frames, stopped, finalized;
static _Atomic(IHS_Thread *) media_worker;
static atomic_uint media_joins;
void __real_IHS_ThreadJoin(IHS_Thread *);
void __wrap_IHS_ThreadJoin(IHS_Thread *thread) {
    bool media = thread && thread == atomic_load(&media_worker);
    __real_IHS_ThreadJoin(thread);
    if (media) {
        atomic_store(&media_worker, NULL);
        atomic_fetch_add(&media_joins, 1);
    }
}
bool __wrap_IHS_BaseSend(IHS_Base *base, IHS_SocketAddress address, const IHS_Buffer *wire) {
    (void)base;
    (void)address;
    (void)wire;
    return true;
}
int __wrap_IHS_UDPSocketWaitReadable(IHS_UDPSocket *socket, uint32_t milliseconds) {
    (void)socket;
    (void)milliseconds;
    return waits++ == 0 && wait_failure ? -1 : 1;
}
int __wrap_IHS_UDPSocketReceive(IHS_UDPSocket *socket, IHS_UDPPacket *received) {
    (void)socket;
    unsigned step = reads++;
    if (!step && !wait_failure)
        return -1;
    if (!wait_failure)
        --step;
    if (step >= 2) {
        usleep(1000);
        return 0;
    }
    unsigned char wire[2272] = {0};
    IHS_Buffer b = IHS_BUFFER_WRAP(wire, sizeof(wire));
    b.size = 0;
    IHS_SessionPacketHeader header = {.type = step ? IHS_SessionPacketTypeUnreliableFEC
                                                   : IHS_SessionPacketTypeConnectACK,
                                      .srcConnectionId = 34,
                                      .dstConnectionId = session->state.connectionId,
                                      .channelId = step ? 9 : 0,
                                      .hasCrc = crc,
                                      .sendTimestamp = IHS_SessionPacketTimestamp()};
    IHS_SessionPacketHeaderSerialize(&header, &b);
    if (!step) {
        IHS_BufferAppendUInt32LE(&b, header.sendTimestamp);
    } else {
        assert(IHS_SessionTransportState(session) == IHS_TransportConnected);
        /* Explicit consumer precondition after the actual CRC transport ACK;
         * this fixture does not authenticate/negotiate a Streaming session. */
        session->state.connectionState = IHS_SessionConnectionStateConnected;
        unsigned shard = sizeof(wire) - 13 - 8 - 4 * crc;
        IHS_BufferAppendUInt32LE(&b, 45); /* one tag +12 header +32 payload */
        IHS_BufferAppendUInt16LE(&b, shard);
        IHS_BufferAppendUInt16LE(&b, 0);
        IHS_BufferAppendUInt8(&b, k_EStreamDataPacket);
        for (unsigned i = 0; i < 12; ++i)
            IHS_BufferAppendUInt8(&b, 0);
        for (unsigned i = 0; i < 32; ++i)
            IHS_BufferAppendUInt8(&b, i);
        while (b.size < sizeof(wire) - 4 * crc)
            IHS_BufferAppendUInt8(&b, 0);
    }
    if (crc) {
        uint32_t checksum = IHS_CRC32C(wire, b.size);
        IHS_BufferAppendUInt32LE(&b, checksum);
    }
    assert(received->buffer.maxCapacity == 2272 && received->buffer.initialCapacity == 2272);
    received->address = session->info.address;
    IHS_BufferAppendMem(&received->buffer, wire, b.size);
    return 1;
}
static void init(IHS_SessionChannel *ch, const void *unused) {
    (void)unused;
    assert(IHS_SessionChannelDataInit(ch, 2048));
}
static bool start(IHS_SessionChannel *ch) {
    (void)ch;
    return true;
}
static void stop(IHS_SessionChannel *ch) {
    (void)ch;
    atomic_fetch_add(&stopped, 1);
}
static void data(IHS_SessionChannel *ch, const IHS_SessionDataFrameHeader *header,
                 IHS_Buffer *body) {
    (void)ch;
    (void)header;
    assert(body->size == 32);
    for (unsigned i = 0; i < 32; ++i)
        assert(IHS_BufferPointer(body)[i] == i);
    assert(atomic_fetch_add(&frames, 1) == 0);
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
static void finished(IHS_Session *s, void *unused) {
    (void)s;
    (void)unused;
    atomic_fetch_add(&finalized, 1);
}
int main(int argc, char **argv) {
    assert(argc == 1 || (argc == 2 && atoi(argv[1]) >= 0 && atoi(argv[1]) < 4));
    alarm(15);
    IHS_Init();
    unsigned first = argc == 2 ? atoi(argv[1]) : 0;
    for (unsigned mode = first; mode < (argc == 2 ? first + 1 : 4); ++mode) {
        wait_failure = mode < 2;
        crc = mode & 1;
        waits = reads = 0;
        atomic_store(&frames, 0);
        atomic_store(&stopped, 0);
        atomic_store(&finalized, 0);
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
        const IHS_StreamSessionCallbacks callbacks = {.finalized = finished};
        IHS_SessionSetSessionCallbacks(session, &callbacks, NULL);
        assert(IHS_SessionConnect(session));
        for (unsigned i = 0; i < 1000 && !atomic_load(&frames) && !atomic_load(&finalized); ++i)
            usleep(1000);
        bool consumed = atomic_load(&frames) == 1;
        IHS_SessionInterrupt(session);
        IHS_SessionThreadedJoin(session);
        if (!consumed || atomic_load(&finalized) != 1)
            fprintf(stderr, "mode=%u wait_calls=%u read_calls=%u frames=%u finalized=%u\n", mode,
                    waits, reads, atomic_load(&frames), atomic_load(&finalized));
        assert(consumed && atomic_load(&finalized) == 1);
        assert(!session->sendThread && !session->base.worker);
        IHS_SessionDestroy(session);
        assert(atomic_load(&stopped) == 1 && atomic_load(&media_joins) == 1 &&
               !atomic_load(&media_worker));
    }
    IHS_Quit();
    alarm(0);
    return 0;
}
