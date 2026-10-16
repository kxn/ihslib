/* Native reliable receive/update entry with controlled time/final UDP.
 * Every observable state and full wire is compared with the official fixture.
 * Poll wrapping only records the raw frame; the real parser still consumes it. */
#include "session/channels/ch_control.h"
#include "session/session_pri.h"
#include "test_session.h"
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint32_t now;
static unsigned result, wireCount, deliveredCount;
static char *wires[16], *delivered[16];
uint32_t __wrap_IHS_SessionPacketTimestamp(void) { return now; }
static char *hex(const uint8_t *p, size_t size) {
    char *s = malloc(size * 2 + 1);
    assert(s);
    for (size_t i = 0; i < size; ++i)
        sprintf(s + i * 2, "%02x", p[i]);
    s[size * 2] = 0;
    return s;
}
bool __wrap_IHS_BaseSend(IHS_Base *base, IHS_SocketAddress address, const IHS_Buffer *data) {
    (void)base;
    (void)address;
    assert(wireCount < 16);
    wires[wireCount++] = hex(IHS_BufferPointer(data), data->size);
    const int errors[] = {0, EAGAIN, EMSGSIZE, EBADF, EIO};
    assert(result <= 4);
    errno = errors[result];
    return !result;
}
bool __real_IHS_SessionPacketsWindowPoll(IHS_SessionPacketsWindow *, IHS_SessionFrame *);
bool __wrap_IHS_SessionPacketsWindowPoll(IHS_SessionPacketsWindow *window,
                                         IHS_SessionFrame *frame) {
    bool ready = __real_IHS_SessionPacketsWindowPoll(window, frame);
    if (ready) {
        assert(deliveredCount < 16);
        delivered[deliveredCount++] = hex(IHS_BufferPointer(&frame->body), frame->body.size);
    }
    return ready;
}
static void array(char **strings, unsigned *count) {
    putchar('[');
    for (unsigned i = 0; i < *count; ++i) {
        printf("%s\"%s\"", i ? "," : "", strings[i]);
        free(strings[i]);
    }
    putchar(']');
    *count = 0;
}
static void observe(IHS_SessionChannelControl *c) {
    printf("{\"wire\":");
    array(wires, &wireCount);
    printf(",\"delivered\":");
    array(delivered, &deliveredCount);
    uint16_t head = IHS_SessionPacketsWindowNextNeededPacketId(c->framePacketWindow);
    uint16_t tail = (uint16_t)(head + IHS_SessionPacketsWindowSize(c->framePacketWindow));
    printf(",\"dirty\":%s,\"pending_ack\":%s,\"confirmed\":%u,"
           "\"peer_time\":%" PRIu32 ",\"peer_received\":%" PRIu32 ","
           "\"deadline\":%" PRIu32 ",\"head\":%u,\"tail\":%u}",
           c->feedbackDirty ? "true" : "false", c->ackPending ? "true" : "false", c->ackWatermark,
           c->peerTimestamp, c->peerReceiveTime, c->feedbackDeadline, head, tail);
}
int main(int argc, char **argv) {
    assert(argc == 4);
    now = strtoul(argv[3], NULL, 10);
    IHS_Init();
    IHS_Session *s = IHS_TestSessionCreate();
    IHS_TimerTaskStopImmediate(s->retransmission.timer);
    s->state.connectionState = IHS_SessionConnectionStateHandshaking;
    s->state.connectionId = 12;
    s->state.hostConnectionId = 34;
    s->state.crcRequired = atoi(argv[1]) != 0;
    s->clock.smoothedRTT = strtoul(argv[2], NULL, 10);
    IHS_SessionChannelControl *c = (void *)s->channels[1];
    printf("{\"initial\":");
    observe(c);
    printf(",\"observed\":[");
    char line[256], op;
    unsigned n = 0;
    while (fgets(line, sizeof(line), stdin)) {
        uint32_t value, id, type, frag, stamp, received;
        assert(sscanf(line, " %c %" SCNu32, &op, &value) == 2);
        if (op == 'S')
            result = value;
        else if (op == 'U') {
            now = value;
            IHS_SessionChannelControlUpdateFeedback(c);
        } else {
            assert(op == 'R');
            int fields = sscanf(
                line, " %c %" SCNu32 " %" SCNu32 " %" SCNu32 " %" SCNu32 " %" SCNu32 " %" SCNu32,
                &op, &value, &id, &type, &frag, &stamp, &received);
            assert(fields == 6 || fields == 7);
            if (fields == 6)
                received = value;
            now = value;
            IHS_SessionPacket p = {.header = {.type = type,
                                              .channelId = 1,
                                              .srcConnectionId = 34,
                                              .dstConnectionId = 12,
                                              .hasCrc = s->state.crcRequired,
                                              .packetId = id,
                                              .fragmentId = frag,
                                              .sendTimestamp = stamp,
                                              .receiveTimestamp = received}};
            IHS_SessionPacketBodyInitialize(&p.body, s->state.crcRequired);
            const uint8_t body[] = {6, 0xaa, 0xbb};
            IHS_BufferAppendMem(&p.body, body, sizeof(body));
            c->base.cls->received(&c->base, &p);
            IHS_SessionPacketClear(&p, true);
        }
        if (n++)
            putchar(',');
        observe(c);
    }
    puts("]}");
    IHS_SessionDestroy(s);
    IHS_Quit();
    return 0;
}
