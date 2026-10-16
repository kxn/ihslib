/* Production frame/control producers -> queue/retention -> Socket serialization.
 * Clock and final UDP are controlled. No sender scheduling or real Steam host. */
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

static unsigned socketCode, sentCount;
static char *sent[16];
uint32_t __wrap_IHS_SessionPacketTimestamp(void) {
    return 1000000;
}
bool __wrap_IHS_BaseSend(IHS_Base *base, IHS_SocketAddress address, const IHS_Buffer *wire) {
    (void)base;
    (void)address;
    assert(sentCount < 16);
    char *str = malloc(wire->size * 2 + 1);
    assert(str);
    for (size_t i = 0; i < wire->size; ++i)
        sprintf(str + 2 * i, "%02x", IHS_BufferPointer(wire)[i]);
    sent[sentCount++] = str;
    const int errors[] = {0, EAGAIN, EMSGSIZE, EBADF, EIO};
    assert(socketCode < 5);
    errno = errors[socketCode];
    return socketCode == 0;
}
int main(int argc, char **argv) {
    assert(argc == 7);
    unsigned crc = (unsigned)atoi(argv[1]), mtu = (unsigned)atoi(argv[2]);
    unsigned kind = (unsigned)atoi(argv[3]), type = (unsigned)atoi(argv[4]);
    socketCode = (unsigned)atoi(argv[5]);
    IHS_Init();
    IHS_Session *s = IHS_TestSessionCreate();
    IHS_TimerTaskStopImmediate(s->retransmission.timer);
    IHS_SessionChannelControl *c = (void *)s->channels[1];
    IHS_TimerTaskStopImmediate(c->feedbackTimer);
    s->state.connectionState = IHS_SessionConnectionStateHandshaking;
    s->state.crcRequired = crc != 0;
    s->state.mtu = mtu;
    s->state.connectionId = 12;
    s->state.hostConnectionId = 34;
    s->outgoing.rate[0] = s->outgoing.rate[1] = 0;
    bool admitted;
    if (kind == 0) {
        IHS_SessionFrame frame;
        IHS_SessionChannelInitializeFrame(&c->base, &frame, IHS_SessionPacketTypeReliable, false,
                                          IHS_PACKET_ID_NEXT);
        frame.body.maxCapacity = 0;
        IHS_BufferAppendUInt8(&frame.body, 6);
        size_t bytes = strlen(argv[6]) / 2;
        for (size_t i = 0; i < bytes; ++i) {
            unsigned byte;
            assert(sscanf(argv[6] + 2 * i, "%2x", &byte) == 1);
            IHS_BufferAppendUInt8(&frame.body, byte);
        }
        admitted = IHS_SessionChannelQueueFrame(&c->base, &frame, true);
        IHS_SessionFrameClear(&frame, true);
    } else if (kind == 2) {
        s->state.connectionState = IHS_SessionConnectionStateConnecting;
        IHS_SessionChannelControlHandshake(&c->base, false);
        admitted = s->state.connectionState == IHS_SessionConnectionStateHandshaking;
    } else {
        CAuthenticationRequestMsg auth = CAUTHENTICATION_REQUEST_MSG__INIT;
        CAuthenticationResponseMsg response = CAUTHENTICATION_RESPONSE_MSG__INIT;
        CStreamingClientHandshakeInfo clientInfo = CSTREAMING_CLIENT_HANDSHAKE_INFO__INIT;
        CClientHandshakeMsg client = CCLIENT_HANDSHAKE_MSG__INIT;
        client.info = &clientInfo;
        CStreamingServerHandshakeInfo serverInfo = CSTREAMING_SERVER_HANDSHAKE_INFO__INIT;
        CServerHandshakeMsg server = CSERVER_HANDSHAKE_MSG__INIT;
        server.info = &serverInfo;
        const ProtobufCMessage *message = type == 1   ? &auth.base
                                          : type == 2 ? &response.base
                                          : type == 6 ? &client.base
                                                      : &server.base;
        admitted = IHS_SessionSendControlMessage(s, type, message);
    }
    typedef struct {
        IHS_SessionPacket packet;
        bool reliable;
    } QueuedPacket;
    QueuedPacket *q;
    int socketResult = -1;
    while ((q = (void *)IHS_QueuePoll(s->sendQueue))) {
        socketResult = IHS_SessionSendPacketResult(s, &q->packet);
        if (q->reliable)
            IHS_RetransmissionNoteInitialSend(&s->retransmission, &q->packet.header,
                                              socketResult == 0, IHS_TimerNow());
        IHS_SessionPacketClear(&q->packet, true);
        IHS_QueueItemFree((void *)q);
    }
    printf("{\"wire\":[");
    for (unsigned i = 0; i < sentCount; ++i) {
        printf("%s\"%s\"", i ? "," : "", sent[i]);
        free(sent[i]);
    }
    printf("],\"admitted\":%s,\"socket_result\":%d,\"next_id\":%u}\n", admitted ? "true" : "false",
           socketResult, s->outgoing.reliableId[1]);
    IHS_SessionDestroy(s);
    IHS_Quit();
    return 0;
}
