#include "test_session.h"
#include "session/packet.h"
#include "ihs_buffer_ext.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <unistd.h>

typedef struct Fixture {
    atomic_int sends, receives, waits, goodbyes;
    atomic_bool finalized, fail;
    bool oversize, reply, terminalAfterAck;
    uint8_t localId;
    unsigned stage;
} Fixture;

static bool Send(void *context, const uint8_t *data, size_t size) {
    Fixture *f = context;
    IHS_SessionPacketHeader h;
    assert(size >= IHS_PACKET_HEADER_SIZE);
    assert(IHS_SessionPacketHeaderParse(&h, data) == IHS_PACKET_HEADER_SIZE);
    if (!atomic_load(&f->sends)) {
        /* The external provider gets the complete original IHS datagram,
         * including Connect's official no-CRC cookie, without a second reliable stream. */
        assert(h.type == IHS_SessionPacketTypeConnect && !h.hasCrc);
        assert(size == IHS_PACKET_HEADER_SIZE + 4);
        assert(!memcmp(data + IHS_PACKET_HEADER_SIZE, "\xc7\x3d\x8f\x3c", 4));
        f->localId = h.srcConnectionId;
    }
    if (h.type == IHS_SessionPacketTypeDisconnect)
        atomic_fetch_add(&f->goodbyes, 1);
    atomic_fetch_add(&f->sends, 1);
    return true;
}
static int Wait(void *context, uint32_t timeoutUs) {
    Fixture *f = context;
    assert(timeoutUs <= 2000);
    atomic_fetch_add(&f->waits, 1);
    if (atomic_load(&f->fail) || (f->reply && atomic_load(&f->sends))) return 1;
    usleep(timeoutUs);
    return 0;
}
static int Receive(void *context, uint8_t *data, size_t capacity, size_t *received, uint32_t timeoutUs) {
    Fixture *f = context;
    assert(capacity == 4096 && timeoutUs == 0);
    atomic_fetch_add(&f->receives, 1);
    if (atomic_load(&f->fail))
        {
            *received = f->oversize ? capacity + 1 : 0;
            return f->oversize ? 1 : -1;
        }
    if (f->reply && atomic_load(&f->sends)) {
        /* SDK permits zero-byte messages. They are consumed, then the same
         * receive batch continues to the following IHS packets. */
        if (f->terminalAfterAck && f->stage == 2)
            return -1;
        if (f->stage++ == 0) {
            *received = 0;
            return 1;
        }
        IHS_SessionPacket packet = {.header = {
            .type = f->stage == 2 ? IHS_SessionPacketTypeConnectACK
                                   : IHS_SessionPacketTypeDisconnect,
            .dstConnectionId = f->localId, .srcConnectionId = 23,
            .channelId = IHS_SessionChannelIdDiscovery,
            .sendTimestamp = IHS_SessionPacketTimestamp()}};
        IHS_SessionPacketBodyInitialize(&packet.body, false);
        if (packet.header.type == IHS_SessionPacketTypeConnectACK)
            IHS_BufferAppendUInt32LE(&packet.body, packet.header.sendTimestamp);
        else
            f->reply = false;
        IHS_SessionPacketPopulateBuffer(&packet);
        size_t size = IHS_SessionPacketSize(&packet);
        assert(size <= capacity);
        memcpy(data, packet.body.data, size);
        IHS_SessionPacketClear(&packet, true);
        *received = size;
        return 1;
    }
    usleep(timeoutUs);
    return 0;
}
static void Finalized(IHS_Session *session, void *context) {
    (void)session;
    atomic_store(&((Fixture *)context)->finalized, true);
}
static void Run(bool reply, bool oversize, bool terminalAfterAck) {
    Fixture f = {.reply = reply, .oversize = oversize, .terminalAfterAck = terminalAfterAck};
    atomic_init(&f.sends, 0);
    atomic_init(&f.goodbyes, 0);
    atomic_init(&f.receives, 0);
    atomic_init(&f.waits, 0);
    atomic_init(&f.finalized, false);
    atomic_init(&f.fail, false);
    IHS_Session *session = IHS_TestSessionCreate();
    IHS_DatagramTransport transport = {.send = Send, .receive = Receive, .wait = Wait,
        .receiveCapacity = 4096};
    assert(!IHS_SessionSetDatagramTransport(session, &(IHS_DatagramTransport){0}, &f));
    assert(IHS_SessionSetDatagramTransport(session, &transport, &f));
    memset(&transport, 0, sizeof(transport)); /* vtable must have been copied */
    IHS_StreamSessionCallbacks callbacks = {.finalized = Finalized};
    IHS_SessionSetSessionCallbacks(session, &callbacks, &f);
    assert(IHS_SessionConnect(session));
    assert(!IHS_SessionSetDatagramTransport(session, NULL, NULL));
    for (int i = 0; i < 200 && (!atomic_load(&f.sends) || !atomic_load(&f.waits)); ++i)
        usleep(1000);
    assert(atomic_load(&f.sends) && atomic_load(&f.waits));
    assert(session->base.socket == NULL);
    if (!reply)
        atomic_store(&f.fail, true); /* terminal receive must wake send worker */
    IHS_SessionThreadedJoin(session);
    assert(atomic_load(&f.finalized));
    assert(IHS_SessionHostRequestedStop(session) == (reply && !terminalAfterAck));
    assert(atomic_load(&f.goodbyes) == (reply ? 1 : 0));
    int sends = atomic_load(&f.sends), receives = atomic_load(&f.receives);
    uint8_t byte = 0;
    IHS_Buffer b = {.data = &byte, .size = 1};
    assert(!IHS_BaseSend(&session->base, sessionInfo.address, &b));
    assert(!IHS_SessionConnect(session));
    IHS_SessionDestroy(session);
    assert(sends == atomic_load(&f.sends) && receives == atomic_load(&f.receives));
}
int main(void) {
    alarm(10);
    IHS_Init();
    /* Fresh sessions prove the provider is not retained by a global worker. */
    for (int i = 0; i < 2; ++i) {
        Run(true, false, false);
        Run(true, false, true);
        Run(false, false, false);
        Run(false, true, false);
    }
    IHS_Session *s = IHS_TestSessionCreate();
    assert(IHS_SessionSetDatagramTransport(s, NULL, NULL));
    IHS_SessionDestroy(s);
    IHS_Quit();
    puts("external datagram transport and join tests passed");
}
