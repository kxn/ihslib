/* Actual reliable receive/poll/feedback. Clock/final UDP and raw frame digest
 * observation are controlled. No reassembly or receiver logic is replaced. */
#include "crypto.h"
#include "session/session_pri.h"
#include "test_session.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint32_t now = 1000000;
static size_t delivered;
static unsigned frames, wires;
static char digest[65], wireHex[2][128];
uint32_t __wrap_IHS_SessionPacketTimestamp(void) {
    return now;
}
IHS_ReliableDisposition __real_IHS_SessionDeliverReliableFrame(IHS_Session *, IHS_SessionFrame *);
IHS_ReliableDisposition __wrap_IHS_SessionDeliverReliableFrame(IHS_Session *s,
                                                               IHS_SessionFrame *f) {
    uint8_t hash[32];
    assert(!IHS_CryptoSHA256(IHS_BufferPointer(&f->body), f->body.size, hash));
    for (unsigned i = 0; i < 32; ++i)
        sprintf(digest + 2 * i, "%02x", hash[i]);
    ++frames;
    delivered = f->body.size;
    return __real_IHS_SessionDeliverReliableFrame(s, f);
}
bool __wrap_IHS_BaseSend(IHS_Base *b, IHS_SocketAddress address, const IHS_Buffer *wire) {
    (void)b;
    (void)address;
    assert(wires < 2 && wire->size * 2 + 1 <= sizeof(wireHex[0]));
    for (size_t i = 0; i < wire->size; ++i)
        sprintf(wireHex[wires] + 2 * i, "%02x", IHS_BufferPointer(wire)[i]);
    ++wires;
    return true;
}
int main(int argc, char **argv) {
    assert(argc == 4);
    IHS_Init();
    unsigned crc = atoi(argv[1]), count = atoi(argv[2]);
    bool missingFirst = atoi(argv[3]);
    assert(count > 0 && count < 16384);
    IHS_Session *s = IHS_TestSessionCreate();
    IHS_TimerTaskStopImmediate(s->retransmission.timer);
    s->state.connectionState = IHS_SessionConnectionStateHandshaking;
    s->state.connectionId = 12;
    s->state.hostConnectionId = 34;
    s->state.crcRequired = crc;
    IHS_ReliableChannel *r = IHS_SessionReliableGet(s, 1);
    now = 1000100;
    for (unsigned i = missingFirst; i < count; ++i) {
        IHS_SessionPacket p = {.header = {.type = i ? 6 : 5,
                                          .channelId = 1,
                                          .packetId = i,
                                          .fragmentId = i ? i - 1 : count - 1,
                                          .srcConnectionId = 34,
                                          .dstConnectionId = 12,
                                          .hasCrc = crc,
                                          .sendTimestamp = 900000,
                                          .receiveTimestamp = now}};
        IHS_SessionPacketBodyInitialize(&p.body, crc);
        uint8_t body[1024];
        memset(body, i % 251, sizeof(body));
        if (!i)
            body[0] = 6;
        IHS_BufferAppendMem(&p.body, body, sizeof(body));
        IHS_SessionReliableReceived(s, &p);
        IHS_SessionPacketClear(&p, true);
    }
    unsigned span = IHS_SessionPacketsWindowSize(r->framePacketWindow);
    unsigned capacity = span + IHS_SessionPacketsWindowAvailable(r->framePacketWindow);
    now = 1000200;
    IHS_SessionReliableUpdateChannel(s, 1);
    assert(frames == (missingFirst ? 0U : 1U) && wires == (missingFirst ? 2U : 1U));
    printf("{\"before\":{\"span\":%u,\"capacity\":%u},\"observed\":{\"wire\":[", span, capacity);
    for (unsigned i = 0; i < wires; ++i)
        printf("%s\"%s\"", i ? "," : "", wireHex[i]);
    printf("],\"delivered\":[");
    if (frames)
        printf("{\"bytes\":%zu,\"sha256\":\"%s\"}", delivered, digest);
    printf("],\"dirty\":%s,\"pending_ack\":%s,\"confirmed\":%u,\"peer_time\":%u,"
           "\"peer_received\":%u,\"deadline\":%u,\"head\":%u,\"tail\":%u}}\n",
           r->feedbackDirty ? "true" : "false", r->ackPending ? "true" : "false", r->ackWatermark,
           r->peerTimestamp, r->peerReceiveTime, r->feedbackDeadline,
           IHS_SessionPacketsWindowNextNeededPacketId(r->framePacketWindow),
           IHS_SessionPacketsWindowNextNeededPacketId(r->framePacketWindow) +
               IHS_SessionPacketsWindowSize(r->framePacketWindow));
    IHS_SessionDestroy(s);
    IHS_Quit();
    return 0;
}
