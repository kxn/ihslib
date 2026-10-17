#include "test_session.h"
#include "session/packet.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <unistd.h>

typedef struct Fixture {
    atomic_int sends, receives;
    atomic_bool finalized, fail;
    bool oversize, reply;
    uint8_t localId;
} Fixture;

static bool Send(void *context, const uint8_t *data, size_t size) {
    Fixture *f = context;
    IHS_SessionPacketHeader h;
    assert(size >= IHS_PACKET_HEADER_SIZE);
    assert(IHS_SessionPacketHeaderParse(&h, data) == IHS_PACKET_HEADER_SIZE);
    if (!atomic_load(&f->sends)) {
        /* The external provider gets the complete original IHS datagram,
         * including Connect's CRC suffix, without a second reliable stream. */
        assert(h.type == IHS_SessionPacketTypeConnect && h.hasCrc);
        assert(size == IHS_PACKET_HEADER_SIZE + 4 + 4);
        assert(!memcmp(data + IHS_PACKET_HEADER_SIZE, "\xc7\x3d\x8f\x3c", 4));
        f->localId = h.srcConnectionId;
    }
    atomic_fetch_add(&f->sends, 1);
    return true;
}
static int Receive(void *context, uint8_t *data, size_t capacity, uint32_t timeoutUs) {
    Fixture *f = context;
    assert(capacity == 2048 && timeoutUs <= 10000);
    atomic_fetch_add(&f->receives, 1);
    if (atomic_load(&f->fail))
        return f->oversize ? (int)capacity + 1 : -1;
    if (f->reply && atomic_load(&f->sends)) {
        f->reply = false;
        IHS_SessionPacketHeader h = {.type = IHS_SessionPacketTypeDisconnect,
            .dstConnectionId = f->localId, .srcConnectionId = 23,
            .channelId = IHS_SessionChannelIdDiscovery};
        IHS_Buffer buffer;
        IHS_BufferInit(&buffer, capacity, capacity);
        IHS_SessionPacketHeaderSerialize(&h, &buffer);
        memcpy(data, IHS_BufferPointer(&buffer), IHS_PACKET_HEADER_SIZE);
        IHS_BufferClear(&buffer, true);
        return IHS_PACKET_HEADER_SIZE;
    }
    usleep(timeoutUs);
    return 0;
}
static void Finalized(IHS_Session *session, void *context) {
    (void)session;
    atomic_store(&((Fixture *)context)->finalized, true);
}
static void Run(bool reply, bool oversize) {
    Fixture f = {.reply = reply, .oversize = oversize};
    atomic_init(&f.sends, 0);
    atomic_init(&f.receives, 0);
    atomic_init(&f.finalized, false);
    atomic_init(&f.fail, false);
    IHS_Session *session = IHS_TestSessionCreate();
    IHS_DatagramTransport transport = {.send = Send, .receive = Receive};
    assert(!IHS_SessionSetDatagramTransport(session, &(IHS_DatagramTransport){0}, &f));
    assert(IHS_SessionSetDatagramTransport(session, &transport, &f));
    memset(&transport, 0, sizeof(transport)); /* vtable must have been copied */
    IHS_StreamSessionCallbacks callbacks = {.finalized = Finalized};
    IHS_SessionSetSessionCallbacks(session, &callbacks, &f);
    assert(IHS_SessionConnect(session));
    assert(!IHS_SessionSetDatagramTransport(session, NULL, NULL));
    for (int i = 0; i < 200 && (!atomic_load(&f.sends) || !atomic_load(&f.receives)); ++i)
        usleep(1000);
    assert(atomic_load(&f.sends) && atomic_load(&f.receives));
    assert(session->base.socket == NULL);
    if (!reply)
        atomic_store(&f.fail, true); /* terminal receive must wake send worker */
    IHS_SessionThreadedJoin(session);
    assert(atomic_load(&f.finalized));
    assert(IHS_SessionHostRequestedStop(session) == reply);
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
        Run(true, false);
        Run(false, false);
        Run(false, true);
    }
    IHS_Session *s = IHS_TestSessionCreate();
    assert(IHS_SessionSetDatagramTransport(s, NULL, NULL));
    IHS_SessionDestroy(s);
    IHS_Quit();
    puts("external datagram transport and join tests passed");
}
