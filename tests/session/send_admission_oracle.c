/* Real producer preparation/admission, codec-independent control serialization,
 * retained reliable state and final socket classification. Only time/final UDP
 * are controlled; no sender worker or host. Local fault modes are independent
 * project protections, not comparisons with official crypto-fault behavior. */
#include "ihs_buffer.h"
#include "ihs_buffer_ext.h"
#include "session/channels/ch_control.h"
#include "session/session_pri.h"
#include "test_session.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned socketCode, attempts, encryptCalls, fault;
uint32_t __wrap_IHS_SessionPacketTimestamp(void) {
    return 1000000;
}
bool __wrap_IHS_BaseSend(IHS_Base *base, IHS_SocketAddress address, const IHS_Buffer *wire) {
    (void)base;
    (void)address;
    (void)wire;
    ++attempts;
    const int errors[] = {0, EAGAIN, EMSGSIZE, EBADF, EIO};
    assert(socketCode < 5);
    errno = errors[socketCode];
    return socketCode == 0;
}
int __real_IHS_SessionFrameEncrypt(IHS_Session *, const uint8_t *, size_t, uint8_t *, size_t *,
                                   uint64_t);
int __wrap_IHS_SessionFrameEncrypt(IHS_Session *s, const uint8_t *in, size_t len, uint8_t *out,
                                   size_t *outlen, uint64_t sequence) {
    ++encryptCalls;
    if (fault == 1)
        return -1;
    return __real_IHS_SessionFrameEncrypt(s, in, len, out, outlen, sequence);
}
bool __real_IHS_RetransmissionTrack(IHS_SessionRetransmission *, const IHS_SessionPacket *,
                                    uint64_t);
bool __wrap_IHS_RetransmissionTrack(IHS_SessionRetransmission *r, const IHS_SessionPacket *p,
                                    uint64_t now) {
    return fault == 2 ? false : __real_IHS_RetransmissionTrack(r, p, now);
}
int main(int argc, char **argv) {
    assert(argc == 7);
    unsigned state = (unsigned)atoi(argv[1]), kind = (unsigned)atoi(argv[2]),
             value = (unsigned)atoi(argv[3]);
    socketCode = (unsigned)atoi(argv[4]);
    bool present = atoi(argv[5]) != 0;
    fault = (unsigned)atoi(argv[6]);
    IHS_Init();
    IHS_Session *s = IHS_TestSessionCreate();
    IHS_TimerTaskStopImmediate(s->retransmission.timer);
    IHS_SessionChannelControl *c = (void *)s->channels[1];
    IHS_TimerTaskStopImmediate(c->feedbackTimer);
    s->state.connectionState =
        state == 0 ? IHS_SessionConnectionStateConnecting : IHS_SessionConnectionStateHandshaking;
    atomic_store(&s->transport.shutdown, state >= 2 ? state - 1 : 0);
    s->state.connectionId = 12;
    s->state.hostConnectionId = 34;
    c->sendEncryptSequence = 7;
    unsigned admission;
    if (kind == 0) {
        IHS_SessionFrame frame;
        IHS_SessionChannelInitializeFrame(&c->base, &frame,
                                          value ? IHS_SessionPacketTypeReliable
                                                : IHS_SessionPacketTypeUnreliable,
                                          false, IHS_PACKET_ID_NEXT);
        const uint8_t body[] = {6, 0xaa, 0xbb};
        IHS_BufferAppendMem(&frame.body, body, sizeof(body));
        IHS_SessionChannel channel = {.session = present ? s : NULL, .id = 1};
        admission = IHS_SessionChannelQueueFrameResult(&channel, &frame, value != 0);
        IHS_SessionFrameClear(&frame, true);
    } else {
        CAuthenticationRequestMsg auth = CAUTHENTICATION_REQUEST_MSG__INIT;
        CAuthenticationResponseMsg response = CAUTHENTICATION_RESPONSE_MSG__INIT;
        CStreamingClientHandshakeInfo clientInfo = CSTREAMING_CLIENT_HANDSHAKE_INFO__INIT;
        CClientHandshakeMsg client = CCLIENT_HANDSHAKE_MSG__INIT;
        client.info = &clientInfo;
        CStreamingServerHandshakeInfo serverInfo = CSTREAMING_SERVER_HANDSHAKE_INFO__INIT;
        CServerHandshakeMsg server = CSERVER_HANDSHAKE_MSG__INIT;
        server.info = &serverInfo;
        CNegotiationInitMsg negotiation = CNEGOTIATION_INIT_MSG__INIT;
        CRemoteHIDMsg remote = CREMOTE_HIDMSG__INIT;
        const ProtobufCMessage *message = value == 1   ? &auth.base
                                          : value == 2 ? &response.base
                                          : value == 6 ? &client.base
                                          : value == 7 ? &server.base
                                          : value == 8 ? &negotiation.base
                                                       : &remote.base;
        bool accepted = IHS_SessionSendControlMessage(s, value, message);
        admission = accepted ? 0 : 3; /* bool API reports acceptance, not sync socket result */
    }
    unsigned queued = 0;
    int socketResult = -1;
    typedef struct {
        IHS_SessionPacket packet;
        bool reliable;
    } QueuedPacket;
    QueuedPacket *q;
    while ((q = (void *)IHS_QueuePoll(s->sendQueue))) {
        ++queued;
        socketResult = IHS_SessionSendPacketResult(s, &q->packet);
        if (q->reliable)
            IHS_RetransmissionNoteInitialSend(&s->retransmission, &q->packet.header,
                                              socketResult == 0, IHS_TimerNow());
        IHS_SessionPacketClear(&q->packet, true);
        IHS_QueueItemFree((void *)q);
    }
    IHS_RetransmissionStats stats;
    IHS_RetransmissionGetStats(&s->retransmission, &stats, IHS_TimerNow());
    printf(
        "{\"state\":%d,\"sequence\":%llu,\"admission\":%u,\"queued\":%u,\"socket_result\":%d,"
        "\"attempts\":%u,\"pending\":%u,\"next_id\":%u,\"encrypt_calls\":%u,\"interrupted\":%s}\n",
        IHS_SessionTransportState(s), (unsigned long long)c->sendEncryptSequence, admission, queued,
        socketResult, attempts, stats.outstanding,
        value || kind ? s->outgoing.reliableId[1] : s->outgoing.unreliableId[1], encryptCalls,
        s->base.interrupted ? "true" : "false");
    IHS_SessionDestroy(s);
    IHS_Quit();
    return 0;
}
