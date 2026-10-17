/* Real data workers receive official early-media schedules after configuration.
 * CRC ingress/windows/FEC/pending are production; only terminal IO/clock and
 * decoder callback are controlled. All consumer threads stop/join explicitly. */
#include "ihs_buffer.h"
#include "ihs_buffer_ext.h"
#include "ihslib.h"
#include "session/channels/ch_data.h"
#include "session/session_pri.h"
#include "test_session.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static atomic_uint tick = 1000000, decoded, feedbacks, goodbyes, started, stopped;
static uint16_t ids[16];
static bool clockMode;
static atomic_uint permits;
static char *metadata[16];
uint32_t __wrap_IHS_SessionPacketTimestamp(void) {
    return atomic_load(&tick);
}
bool __wrap_IHS_BaseSend(IHS_Base *base, IHS_SocketAddress peer, const IHS_Buffer *wire) {
    (void)base;
    (void)peer;
    assert(wire->size >= 13);
    unsigned type = *IHS_BufferPointer(wire) & 127;
    if (type == IHS_SessionPacketTypeDisconnect) {
        atomic_fetch_add(&goodbyes, 1);
        return true;
    }
    assert(type == IHS_SessionPacketTypeACK || type == IHS_SessionPacketTypeNACK ||
           type == IHS_SessionPacketTypeUnreliableNack);
    atomic_fetch_add(&feedbacks, 1);
    return true;
}
static void init(IHS_SessionChannel *ch, const void *unused) {
    (void)unused;
    assert(IHS_SessionChannelDataInit(ch, 32));
}
static bool start(IHS_SessionChannel *ch) {
    (void)ch;
    atomic_fetch_add(&started, 1);
    while (clockMode && !atomic_load(&permits) && !((IHS_SessionChannelData *)ch)->interrupted)
        usleep(1000);
    return true; /* This fixture models successful start, followed by stop even on early cancel. */
}
static void stop(IHS_SessionChannel *ch) {
    (void)ch;
    atomic_fetch_add(&stopped, 1);
}
static void data(IHS_SessionChannel *ch, const IHS_SessionDataFrameHeader *h, IHS_Buffer *body) {
    assert(ch->id == 3 && h->id >= 1 && h->id <= 2 && body->size == 87);
    for (size_t i = 7; i < body->size; ++i)
        assert(IHS_BufferPointer(body)[i] == h->id);
    unsigned n = atomic_load(&decoded);
    assert(n < 16);
    ids[n] = h->id;
    if (clockMode) {
        char value[512];
        snprintf(value, sizeof(value),
                 "{\"id\":%u,\"frame\":%u,\"sent\":%u,\"received\":%u,\"first\":%u,\"input\":%u,"
                 "\"input_received\":%u,\"size\":%zu}",
                 h->id, h->timestamp, h->sendTimestamp, h->receiveTimestamp, h->receiveBegin,
                 h->inputMark, h->inputRecvTimestamp, body->size);
        metadata[n] = strdup(value);
        assert(metadata[n]);
    }
    atomic_store(&decoded, n + 1);
    while (clockMode && n == 0 && atomic_load(&permits) <= 1 &&
           !((IHS_SessionChannelData *)ch)->interrupted)
        usleep(1000);
}
static const IHS_SessionChannelDataClass cls = {
    .base = {.init = init,
             .deinit = IHS_SessionChannelDataDeinit,
             .received = IHS_SessionChannelDataReceived,
             .stopped = IHS_SessionChannelDataStopped,
             .instanceSize = sizeof(IHS_SessionChannelData)},
    .start = start,
    .stop = stop,
    .dataFrame = data,
};
int main(int argc, char **argv) {
    assert(argc == 2 || argc == 3);
    clockMode = argc == 3;
    IHS_Init();
    IHS_Session *s = IHS_TestSessionCreate();
    IHS_TimerTaskStopImmediate(s->retransmission.timer);
    s->state.connectionState = IHS_SessionConnectionStateConnected;
    s->state.connectionId = 12;
    s->state.hostConnectionId = 34;
    s->state.crcRequired = atoi(argv[1]);
    IHS_OutgoingRegister(s, 3);
    IHS_UnreliableChannel *initial = IHS_SessionUnreliableGet(s, 3, 2048);
    assert(initial);
    IHS_SessionChannel *consumer = NULL;
    char line[8192];
    while (fgets(line, sizeof(line), stdin)) {
        unsigned when, kind, channel, id, retry, fragment, sent;
        if (line[0] == 'W') {
            char encoded[4096];
            assert(sscanf(line, "W %u %u %u %u %u %u %u %4095s", &when, &kind, &channel, &id,
                          &retry, &fragment, &sent, encoded) == 8);
            atomic_store(&tick, when);
            IHS_SessionPacket p = {.header = {.type = kind,
                                              .channelId = channel,
                                              .srcConnectionId = 34,
                                              .dstConnectionId = 12,
                                              .hasCrc = s->state.crcRequired,
                                              .packetId = id,
                                              .retransmitCount = retry,
                                              .fragmentId = fragment,
                                              .sendTimestamp = sent}};
            IHS_SessionPacketBodyInitialize(&p.body, p.header.hasCrc);
            for (size_t i = 0; encoded[i]; i += 2) {
                unsigned value;
                assert(sscanf(encoded + i, "%2x", &value) == 1);
                IHS_BufferAppendUInt8(&p.body, value);
            }
            IHS_SessionPacketPopulateBuffer(&p);
            IHS_Buffer wire = IHS_BUFFER_INIT(2048, 2048);
            IHS_BufferAppendMem(&wire, p.body.data, IHS_SessionPacketSize(&p));
            s->base.callbacks.received(&s->base, &s->info.address, &wire);
            IHS_BufferClear(&wire, true);
            IHS_SessionPacketClear(&p, true);
        } else if (line[0] == 'X') {
            IHS_SessionPacket p = {.header = {.type = IHS_SessionPacketTypeDisconnect,
                                              .srcConnectionId = 34,
                                              .dstConnectionId = 12,
                                              .hasCrc = s->state.crcRequired,
                                              .sendTimestamp = atomic_load(&tick)}};
            IHS_SessionPacketBodyInitialize(&p.body, p.header.hasCrc);
            IHS_SessionPacketPopulateBuffer(&p);
            IHS_Buffer wire = IHS_BUFFER_INIT(2048, 2048);
            IHS_BufferAppendMem(&wire, p.body.data, IHS_SessionPacketSize(&p));
            s->base.callbacks.received(&s->base, &s->info.address, &wire);
            IHS_BufferClear(&wire, true);
            IHS_SessionPacketClear(&p, true);
        } else if (line[0] == 'U') {
            assert(sscanf(line, "U %u %u", &when, &channel) == 2);
            atomic_store(&tick, when);
            IHS_SessionNetworkUpdateAt(s, when);
        } else if (line[0] == 'C') {
            assert(sscanf(line, "C %u", &channel) == 1);
            if (consumer)
                IHS_SessionChannelRemove(s, consumer->id);
            assert(s->unreliable[3] == initial);
            assert(IHS_SessionUnreliableSetDelay(s, channel, 6553));
            consumer = IHS_SessionChannelDataCreate(&cls, s, IHS_SessionChannelTypeDataVideo,
                                                    channel, NULL);
            assert(consumer);
            IHS_SessionChannelAdd(s, consumer);
        } else if (line[0] == 'Q') {
            unsigned phase, paused;
            assert(sscanf(line, "Q %u %u", &phase, &paused) == 2);
            s->state.connectionState = phase;
            s->transport.clientPaused = paused;
        } else if (line[0] == 'O') {
            unsigned value;
            assert(sscanf(line, "O %u", &value) == 1);
            IHS_MutexLock(s->clock.lock);
            s->clock.offset = (int32_t)value;
            IHS_MutexUnlock(s->clock.lock);
        } else if (line[0] == 'T') {
            unsigned echo, peer;
            assert(sscanf(line, "T %u %u %u", &echo, &peer, &when) == 3);
            atomic_store(&tick, when);
            IHS_StreamClockFeedback(&s->clock, echo, peer, when);
        } else if (line[0] == 'D') {
            unsigned count;
            assert(sscanf(line, "D %u", &count) == 1);
            if (clockMode)
                atomic_store(&permits, count);
            uint64_t until = IHS_TimerNow() + 1000;
            while (atomic_load(&decoded) < count && IHS_TimerNow() < until)
                usleep(1000);
            assert(atomic_load(&decoded) == count);
        } else {
            assert(!"unknown worker command");
        }
    }
    if (consumer)
        IHS_SessionChannelRemove(s, consumer->id);
    assert(s->unreliable[3] == initial && atomic_load(&started) == atomic_load(&stopped));
    IHS_SessionDestroy(s);
    IHS_Quit();
    printf("{\"decoded\":[");
    for (unsigned i = 0; i < atomic_load(&decoded); ++i)
        printf("%s%u", i ? "," : "", ids[i]);
    if (clockMode) {
        printf("],\"metadata\":[");
        for (unsigned i = 0; i < atomic_load(&decoded); ++i) {
            printf("%s%s", i ? "," : "", metadata[i]);
            free(metadata[i]);
        }
    }
    printf("],\"feedbacks\":%u,\"goodbyes\":%u,\"started\":%u,\"stopped\":%u}\n",
           atomic_load(&feedbacks), atomic_load(&goodbyes), atomic_load(&started),
           atomic_load(&stopped));
    return 0;
}
