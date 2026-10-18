/* Real Connect serializer, receive guards, handshake and receive-worker update.
 * Only clock, final UDP result and final disconnect cleanup are controlled. */
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

static uint32_t now;
static unsigned result;
static char actions[4096];
uint32_t __wrap_IHS_SessionPacketTimestamp(void) { return now; }
bool __wrap_IHS_BaseSend(IHS_Base *base, IHS_SocketAddress addr, const IHS_Buffer *wire) {
    (void)base;
    (void)addr;
    size_t offset = strlen(actions);
    offset += (size_t)sprintf(actions + offset, "%s{\"action\":\"send\",\"wire\":\"",
                             offset ? "," : "");
    assert(offset + wire->size * 2 + 4 < sizeof(actions));
    for (size_t i = 0; i < wire->size; ++i)
        sprintf(actions + offset + 2 * i, "%02x", IHS_BufferPointer(wire)[i]);
    strcat(actions, "\"}");
    errno = result == 1 ? EAGAIN : EIO;
    return result == 0;
}
void __wrap_IHS_SessionDisconnect(IHS_Session *s) {
    (void)s;
    strcat(actions, actions[0] ? ",{\"action\":\"disconnect\"}" : "{\"action\":\"disconnect\"}");
}
void __wrap_IHS_SessionTransportDisconnect(IHS_Session *s) {
    __wrap_IHS_SessionDisconnect(s);
}
static void print(IHS_Session *s) {
    IHS_RetransmissionStats stats;
    IHS_RetransmissionGetStats(&s->retransmission, &stats, IHS_TimerNow());
    unsigned state = s->transport.timedOut ? 2 :
        s->state.connectionState >= IHS_SessionConnectionStateHandshaking ? 1 : 0;
    printf("{\"state\":%u,\"last_connect\":%u,\"crc_required\":%s,\"actions\":[%s],"
           "\"reliable_outstanding\":%u}\n", state, s->transport.lastConnect,
           s->state.crcRequired ? "true" : "false", actions, stats.outstanding);
    actions[0] = 0;
}
int main(void) {
    IHS_Init();
    IHS_Session *s = IHS_TestSessionCreate();
    IHS_TimerTaskStopImmediate(s->retransmission.timer);
    char line[128];
    unsigned paused, size, crc;
    while (fgets(line, sizeof(line), stdin)) {
        if (sscanf(line, "START %u %u", &now, &result) == 2) {
            s->state.connectionState = IHS_SessionConnectionStateConnecting;
            s->state.connectionId = 12;
            s->state.hostConnectionId = 0;
            s->state.crcRequired = false;
            IHS_SessionTransportBegin(s, now);
            IHS_SessionTransportSendConnect(s);
        } else if (sscanf(line, "UPDATE %u", &now) == 1) {
            s->base.callbacks.run->update(&s->base, NULL);
        } else if (sscanf(line, "PAUSE %u", &paused) == 1) {
            s->transport.connectionPaused = paused;
            continue;
        } else if (sscanf(line, "CONNECTED %u %u", &size, &crc) == 2) {
            IHS_SessionPacket p = {.header = {.type = IHS_SessionPacketTypeConnectACK,
                .srcConnectionId = 34, .dstConnectionId = 12, .hasCrc = crc,
                .sendTimestamp = now}};
            IHS_SessionPacketBodyInitialize(&p.body, crc);
            assert(size <= 5);
            uint8_t body[5] = {0};
            IHS_BufferAppendMem(&p.body, body, size);
            IHS_SessionPacketPopulateBuffer(&p);
            IHS_Buffer data;
            IHS_BufferTransferOwnership(&p.body, &data);
            IHS_BufferExtendSize(&data);
            s->base.callbacks.received(&s->base, &s->info.address, &data);
            IHS_BufferClear(&data, true);
            IHS_SessionPacketClear(&p, true);
            typedef struct { IHS_SessionPacket packet; bool reliable; } QueuedPacket;
            QueuedPacket *q;
            while ((q = (void *)IHS_QueuePoll(s->sendQueue))) {
                assert(q->packet.header.type == 5 && q->packet.header.channelId == 1);
                assert(q->packet.body.size && *IHS_BufferPointer(&q->packet.body) == 6);
                strcat(actions, "{\"action\":\"connected\"}");
                IHS_SessionPacketClear(&q->packet, true);
                IHS_QueueItemFree((void *)q);
            }
        } else {
            assert(!"unknown command");
        }
        print(s);
    }
    IHS_SessionDestroy(s);
    IHS_Quit();
    return 0;
}
