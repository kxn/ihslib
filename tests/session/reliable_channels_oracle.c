/* Actual wire ingress, generic reliable receiver, client dispatch, media worker.
 * Clock/final UDP and the terminal decoder are controlled; no transport or
 * reassembly is replaced. The observer records real frames and queue contents. */
#include "ihs_buffer.h"
#include "ihs_buffer_ext.h"
#include "session/channels/ch_control.h"
#include "session/channels/ch_data.h"
#include "session/session_pri.h"
#include "test_session.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static atomic_uint now = 1000000;
static unsigned target, wireCount, deliveredCount, actionCount, expectedData;
static char *wires[32], *delivered[32], *actions[32];
static struct {
    uint32_t send, receive;
    char *raw;
} rawFrames[32];
static unsigned rawCount;
static atomic_uint dataCount;
static pthread_mutex_t observerLock = PTHREAD_MUTEX_INITIALIZER;
uint32_t __wrap_IHS_SessionPacketTimestamp(void) { return now; }
static char *hex(const uint8_t *p, size_t size) {
    char *s = malloc(2 * size + 1);
    assert(s);
    for (size_t i = 0; i < size; ++i)
        sprintf(s + 2 * i, "%02x", p[i]);
    s[2 * size] = 0;
    return s;
}
static void append(const char *action) {
    assert(actionCount < 32);
    actions[actionCount++] = strdup(action);
}
bool __wrap_IHS_BaseSend(IHS_Base *b, IHS_SocketAddress address, const IHS_Buffer *data) {
    (void)b;
    (void)address;
    assert(wireCount < 32);
    wires[wireCount++] = hex(IHS_BufferPointer(data), data->size);
    return true;
}
IHS_ReliableDisposition __real_IHS_SessionDeliverReliableFrame(IHS_Session *, IHS_SessionFrame *);
IHS_ReliableDisposition __wrap_IHS_SessionDeliverReliableFrame(IHS_Session *s,
                                                               IHS_SessionFrame *f) {
    char *raw = hex(IHS_BufferPointer(&f->body), f->body.size);
    pthread_mutex_lock(&observerLock);
    assert(rawCount < 32 && deliveredCount < 32);
    rawFrames[rawCount++] =
        (typeof(rawFrames[0])){f->header.sendTimestamp, f->header.receiveTimestamp, strdup(raw)};
    delivered[deliveredCount++] = strdup(raw);
    pthread_mutex_unlock(&observerLock);
    IHS_ReliableDisposition result = __real_IHS_SessionDeliverReliableFrame(s, f);
    if (result == IHS_ReliableIgnored) {
        char line[4096];
        snprintf(line, sizeof(line), "{\"ignored\":\"%s\"}", raw);
        pthread_mutex_lock(&observerLock);
        append(line);
        pthread_mutex_unlock(&observerLock);
    }
    free(raw);
    return result;
}
void __real_IHS_SessionChannelDataReceiveReliableFrame(IHS_SessionChannel *, IHS_SessionFrame *);
void __wrap_IHS_SessionChannelDataReceiveReliableFrame(IHS_SessionChannel *c, IHS_SessionFrame *f) {
    /* Pending drain calls the dispatcher within its translation unit. Observe
     * this second real handoff too; no receiver/queue implementation is replaced. */
    char *raw = hex(IHS_BufferPointer(&f->body), f->body.size);
    pthread_mutex_lock(&observerLock);
    bool present = false;
    for (unsigned i = 0; i < rawCount; ++i)
        if (rawFrames[i].send == f->header.sendTimestamp &&
            rawFrames[i].receive == f->header.receiveTimestamp && !strcmp(rawFrames[i].raw, raw))
            present = true;
    if (!present) {
        assert(rawCount < 32 && deliveredCount < 32);
        rawFrames[rawCount++] = (typeof(rawFrames[0])){f->header.sendTimestamp,
                                                       f->header.receiveTimestamp, strdup(raw)};
        delivered[deliveredCount++] = strdup(raw);
    }
    pthread_mutex_unlock(&observerLock);
    free(raw);
    ++expectedData;
    __real_IHS_SessionChannelDataReceiveReliableFrame(c, f);
}
static void init(IHS_SessionChannel *c, const void *cfg) {
    (void)cfg;
    IHS_SessionChannelDataInit(c, 32);
}
static bool start(IHS_SessionChannel *c) {
    (void)c;
    return true;
}
static void stop(IHS_SessionChannel *c) { (void)c; }
static void data(IHS_SessionChannel *c, const IHS_SessionDataFrameHeader *h, IHS_Buffer *body) {
    (void)c;
    pthread_mutex_lock(&observerLock);
    const char *raw = NULL;
    for (unsigned i = 0; i < rawCount; ++i)
        if (rawFrames[i].send == h->sendTimestamp && rawFrames[i].receive == h->receiveTimestamp)
            raw = rawFrames[i].raw;
    assert(raw && strlen(raw) >= 26);
    char *payload = hex(IHS_BufferPointer(body), body->size);
    assert(!strcmp(payload, raw + 26));
    free(payload);
    char line[4096];
    snprintf(line, sizeof(line),
             "{\"data_type\":1,\"frame\":\"%s\",\"id\":%u,"
             "\"timestamp\":%u,\"send_timestamp\":%u,\"receive_timestamp\":%u,"
             "\"input_mark\":%u,\"input_received\":%u}",
             raw, h->id, h->timestamp, h->sendTimestamp, h->receiveTimestamp, h->inputMark,
             h->inputRecvTimestamp);
    append(line);
    pthread_mutex_unlock(&observerLock);
    atomic_fetch_add(&dataCount, 1);
}
static const IHS_SessionChannelDataClass cls = {
    .base = {.init = init,
             .deinit = IHS_SessionChannelDataDeinit,
             .received = IHS_SessionChannelDataReceived,
             .stopped = IHS_SessionChannelDataStopped,
             .instanceSize = sizeof(IHS_SessionChannelData)},
    .start = start,
    .dataFrame = data,
    .stop = stop,
};
static void array(char **items, unsigned *count, bool quote) {
    putchar('[');
    for (unsigned i = 0; i < *count; ++i) {
        printf("%s%s%s%s", i ? "," : "", quote ? "\"" : "", items[i], quote ? "\"" : "");
        free(items[i]);
    }
    putchar(']');
    *count = 0;
}
static void state(IHS_Session *s) {
    if (!s->outgoing.active[target]) {
        fputs("null", stdout);
        return;
    }
    IHS_ReliableChannel *c = &s->reliable[target];
    uint16_t head =
        c->framePacketWindow ? IHS_SessionPacketsWindowNextNeededPacketId(c->framePacketWindow) : 0;
    uint16_t tail = c->framePacketWindow
                        ? (uint16_t)(head + IHS_SessionPacketsWindowSize(c->framePacketWindow))
                        : 0;
    printf("{\"wire\":");
    array(wires, &wireCount, true);
    printf(",\"delivered\":");
    array(delivered, &deliveredCount, true);
    printf(",\"dirty\":%s,\"pending_ack\":%s,\"confirmed\":%u,\"peer_time\":%u,"
           "\"peer_received\":%u,\"deadline\":%u,\"head\":%u,\"tail\":%u,\"last_nack\":%u}",
           c->feedbackDirty ? "true" : "false", c->ackPending ? "true" : "false", c->ackWatermark,
           c->peerTimestamp, c->peerReceiveTime, c->feedbackDeadline, head, tail,
           c->lastNackTimestamp);
}
static void observe(IHS_Session *s) {
    for (unsigned i = 0; atomic_load(&dataCount) != expectedData && i < 1000; ++i)
        usleep(1000);
    assert(atomic_load(&dataCount) == expectedData);
    printf("{\"state\":");
    state(s);
    printf(",\"actions\":");
    pthread_mutex_lock(&observerLock);
    array(actions, &actionCount, false);
    for (unsigned i = 0; i < rawCount; ++i)
        free(rawFrames[i].raw);
    rawCount = 0;
    pthread_mutex_unlock(&observerLock);
    printf(",\"channels\":[");
    for (unsigned i = 0; i < s->outgoing.count; ++i)
        printf("%s%u", i ? "," : "", s->outgoing.creation[i]);
    printf("],\"pending\":[");
    /* Snapshot the real RX-owned pending queue, restoring its order. */
    struct Prefix {
        IHS_SessionPacket packet;
    };
    unsigned n = s->pendingDataCount;
    for (unsigned i = 0; i < n; ++i) {
        IHS_QueueItem *item = IHS_QueuePoll(s->pendingData);
        assert(item);
        struct Prefix *p = (void *)item;
        char *body = hex(IHS_BufferPointer(&p->packet.body), p->packet.body.size);
        printf("%s\"%s\"", i ? "," : "", body);
        free(body);
        IHS_QueueAppend(s->pendingData, item);
    }
    printf("]}");
}
static void bytes(IHS_Buffer *body, const char *text) {
    if (!strcmp(text, "-"))
        return;
    unsigned byte;
    for (size_t i = 0; text[i]; i += 2) {
        assert(text[i + 1] && sscanf(text + i, "%2x", &byte) == 1);
        IHS_BufferAppendUInt8(body, byte);
    }
}
int main(int argc, char **argv) {
    assert(argc == 6);
    IHS_Init();
    unsigned crc = atoi(argv[1]), phase = atoi(argv[3]);
    target = atoi(argv[2]);
    IHS_Session *s = IHS_TestSessionCreate();
    IHS_TimerTaskStopImmediate(s->retransmission.timer);
    s->state.connectionId = 12;
    s->state.hostConnectionId = 34;
    s->state.crcRequired = crc;
    s->state.connectionState = phase == 0   ? IHS_SessionConnectionStateUnconnected
                               : phase == 2 ? IHS_SessionConnectionStateHandshaking
                                            : IHS_SessionConnectionStateConnected;
    s->transport.shutdown = phase == 7;
    s->clock.smoothedRTT = 100;
    s->outgoing.rate[0] = s->outgoing.rate[1] = 0;
    IHS_SessionReliableGet(s, 1); /* The source fixture starts after the 0/1 factory bootstrap. */
    if (atoi(argv[5])) {
        IHS_SessionChannel *c =
            IHS_SessionChannelDataCreate(&cls, s, IHS_SessionChannelTypeDataAudio, target, NULL);
        assert(c);
        IHS_SessionChannelAdd(s, c);
    }
    printf("{\"initial\":");
    state(s);
    printf(",\"observed\":[");
    unsigned step = 0, id, type, frag, stamp, opTime;
    char line[4096], op, encoded[2048];
    while (fgets(line, sizeof(line), stdin)) {
        assert(sscanf(line, " %c %u", &op, &opTime) == 2);
        atomic_store(&now, opTime);
        if (op == 'U') {
            IHS_SessionReliableUpdateChannel(s, target);
            IHS_OutgoingUpdate(s, now);
        } else if (op == 'C') {
            unsigned configured = target;
            sscanf(line, " %c %u %u", &op, &opTime, &configured);
            IHS_SessionChannel *previous = IHS_SessionChannelForType(s, IHS_SessionChannelTypeDataAudio);
            if (previous && previous->id != configured)
                IHS_SessionChannelRemove(s, previous->id);
            IHS_SessionChannel *c = IHS_SessionChannelFor(s, configured);
            if (c) {
                IHS_SessionDrainPendingData(s, c);
            } else {
                c = IHS_SessionChannelDataCreate(&cls, s, IHS_SessionChannelTypeDataAudio, configured, NULL);
                assert(c);
                IHS_SessionChannelAdd(s, c);
            }
        } else if (op == 'P' || op == 'T') {
            unsigned value;
            assert(sscanf(line, " %c %u %u", &op, &opTime, &value) == 3);
            if (op == 'P') s->transport.clientPaused = value;
            else s->state.connectionState = value;
        } else if (op == 'F') {
            assert(sscanf(line, " %c %u %2047s", &op, &opTime, encoded) == 3);
            IHS_SessionChannel endpoint = {.session = s, .id = target};
            IHS_SessionFrame f;
            IHS_SessionChannelInitializeFrame(&endpoint, &f, IHS_SessionPacketTypeReliable, false,
                                              IHS_PACKET_ID_NEXT);
            IHS_BufferAppendUInt8(&f.body, 6);
            bytes(&f.body, encoded);
            assert(IHS_SessionChannelQueueFrame(&endpoint, &f, true));
            IHS_SessionFrameClear(&f, true);
            assert(IHS_SessionDispatchQueued(s, false));
        } else {
            IHS_SessionPacket p = {.header = {.channelId = target,
                                              .srcConnectionId = 34,
                                              .dstConnectionId = 12,
                                              .hasCrc = crc}};
            IHS_SessionPacketBodyInitialize(&p.body, crc);
            if (op == 'N') {
                assert(sscanf(line, " %c %u %u %u %2047s", &op, &opTime, &id, &stamp, encoded) ==
                       5);
                p.header.type = IHS_SessionPacketTypeNACK;
                p.header.packetId = id;
                p.header.sendTimestamp = stamp;
                bytes(&p.body, encoded);
            } else {
                unsigned received;
                int fields = sscanf(line, " %c %u %u %u %u %u %u %2047s", &op, &opTime, &id, &type,
                                    &frag, &stamp, &received, encoded);
                assert(op == 'R' && (fields == 6 || fields == 8));
                p.header.type = type;
                p.header.packetId = id;
                p.header.fragmentId = frag;
                p.header.sendTimestamp = stamp;
                bytes(&p.body, fields == 8 ? encoded : argv[4]);
            }
            IHS_SessionPacketPopulateBuffer(&p);
            IHS_Buffer wire = IHS_BUFFER_INIT(2048, 2048);
            IHS_BufferAppendMem(&wire, p.body.data, IHS_SessionPacketSize(&p));
            s->base.callbacks.received(&s->base, &s->info.address, &wire);
            IHS_BufferClear(&wire, true);
            IHS_SessionPacketClear(&p, true);
        }
        if (step++)
            putchar(',');
        observe(s);
    }
    puts("]}");
    IHS_SessionDestroy(s);
    IHS_Quit();
    return 0;
}
