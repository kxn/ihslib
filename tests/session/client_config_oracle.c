/* Real session wire ingress, crypto/reassembly, complete config and queued 99.
 * Only lifecycle I/O, application delegate/persistence and host input are controlled. */
#include "ihs_buffer.h"
#include "ihs_buffer_ext.h"
#include "session/channels/ch_control.h"
#include "session/session_pri.h"
#include "test_session.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    IHS_SessionPacket packet;
    bool reliable;
} QueuedPacket;
static bool emitting, first;
static uint64_t incomingSequence, outgoingSequence;
static uint16_t incomingId;
static IHS_SessionPacketsWindow *outgoing;

static void hex(const uint8_t *p, size_t n) {
    putchar('"');
    for (size_t i = 0; i < n; ++i)
        printf("%02x", p[i]);
    putchar('"');
}
static void event(void) {
    if (!first)
        putchar(',');
    first = false;
}
static void changed(IHS_Session *s, const uint8_t *p, size_t n, bool local, void *ctx) {
    (void)s;
    (void)ctx;
    if (!emitting)
        return;
    event();
    fputs("{\"notify\":", stdout);
    hex(p, n);
    printf(",\"local_only\":%s}", local ? "true" : "false");
}
static void save(IHS_Session *s, const uint8_t *p, size_t n, void *ctx) {
    (void)s;
    (void)ctx;
    if (!emitting)
        return;
    event();
    fputs("{\"save\":", stdout);
    hex(p, n);
    putchar('}');
}
static void drain(IHS_Session *s) {
    QueuedPacket *q;
    while ((q = (void *)IHS_QueuePoll(s->sendQueue))) {
        if (q->packet.header.type == IHS_SessionPacketTypeReliable ||
            q->packet.header.type == IHS_SessionPacketTypeReliableFrag) {
            assert(IHS_SessionPacketSize(&q->packet) <= (unsigned)s->state.mtu);
            assert(IHS_SessionPacketsWindowAdd(outgoing, &q->packet));
        }
        IHS_SessionPacketClear(&q->packet, true);
        IHS_QueueItemFree((void *)q);
        IHS_SessionFrame frame = {.body = IHS_BUFFER_INIT(1024, 0)};
        while (IHS_SessionPacketsWindowPoll(outgoing, &frame)) {
            unsigned type = *IHS_BufferPointer(&frame.body);
            assert(type == k_EStreamControlSetStreamingClientConfig ||
                   type == k_EStreamControlNegotiationSetConfig);
            IHS_BufferOffsetBy(&frame.body, 1);
            IHS_Buffer plain = IHS_BUFFER_INIT(1024, 0);
            uint64_t seq;
            assert(IHS_SessionFrameDecrypt(s, &frame.body, &plain, outgoingSequence++, &seq) ==
                   IHS_SessionFrameDecryptOK);
            if (emitting) {
                event();
                printf("{\"send\":%u,\"payload\":", type);
                hex(IHS_BufferPointer(&plain), plain.size);
                putchar('}');
            }
            IHS_BufferClear(&plain, true);
            IHS_SessionFrameClear(&frame, true);
            frame = (IHS_SessionFrame){.body = IHS_BUFFER_INIT(1024, 0)};
        }
    }
}
static void receive(IHS_Session *s, const uint8_t *payload, size_t n) {
    size_t cipherSize = n + 64;
    uint8_t *body = malloc(cipherSize + 1);
    assert(body);
    body[0] = k_EStreamControlSetStreamingClientConfig;
    assert(IHS_SessionFrameEncrypt(s, payload, n, body + 1, &cipherSize, incomingSequence++) == 0);
    size_t total = cipherSize + 1, limit = 495, count = (total + limit - 1) / limit;
    for (size_t i = 0, offset = 0; i < count; ++i) {
        IHS_SessionPacket p = {.header = {.type = i ? IHS_SessionPacketTypeReliableFrag
                                                    : IHS_SessionPacketTypeReliable,
                                          .channelId = 1,
                                          .srcConnectionId = 34,
                                          .dstConnectionId = 12,
                                          .hasCrc = true,
                                          .packetId = incomingId++,
                                          .fragmentId = i ? (int)i - 1 : (int)count - 1,
                                          .sendTimestamp = 900000}};
        IHS_SessionPacketBodyInitialize(&p.body, true);
        size_t size = total - offset;
        if (size > limit)
            size = limit;
        IHS_BufferAppendMem(&p.body, body + offset, size);
        offset += size;
        IHS_SessionPacketPopulateBuffer(&p);
        IHS_Buffer wire = IHS_BUFFER_INIT(2048, 2048);
        IHS_BufferAppendMem(&wire, p.body.data, IHS_SessionPacketSize(&p));
        s->base.callbacks.received(&s->base, &s->info.address, &wire);
        IHS_BufferClear(&wire, true);
        IHS_SessionPacketClear(&p, true);
    }
    free(body);
}
int main(void) {
    IHS_Init();
    IHS_Session *s = IHS_TestSessionCreate();
    assert(s);
    IHS_TimerTaskStopImmediate(s->retransmission.timer);
    IHS_TimerTaskStopImmediate(((IHS_SessionChannelControl *)s->channels[1])->feedbackTimer);
    s->state.connectionId = 12;
    s->state.hostConnectionId = 34;
    s->state.mtu = 512;
    const IHS_StreamSessionCallbacks callbacks = {.clientConfigChanged = changed,
                                                  .saveClientConfig = save};
    IHS_SessionSetSessionCallbacks(s, &callbacks, NULL);
    outgoing = IHS_SessionPacketsWindowCreateReliable(320, 0);
    char *line = NULL;
    size_t cap = 0;
    while (getline(&line, &cap, stdin) >= 0) {
        unsigned phase, local;
        if (sscanf(line, "PHASE %u", &phase) == 1) {
            s->state.connectionState = phase;
            continue;
        }
        char *value = strchr(line, ' ');
        assert(value);
        value++;
        bool seed = !strncmp(line, "SEED ", 5), set = !strncmp(line, "SET ", 4);
        local = 1;
        if (set) {
            assert(sscanf(value, "%u", &local) == 1);
            value = strchr(value, ' ') + 1;
        }
        size_t len = strcspn(value, "\r\n");
        if (len == 1 && value[0] == '-')
            len = 0;
        assert(!(len & 1));
        uint8_t *bytes = malloc(len / 2 + 1);
        assert(bytes);
        for (size_t i = 0; i < len / 2; ++i) {
            unsigned b;
            assert(sscanf(value + 2 * i, "%2x", &b) == 1);
            bytes[i] = b;
        }
        emitting = !seed;
        first = true;
        if (emitting)
            fputs("{\"events\":[", stdout);
        bool result = true;
        if (seed || set)
            result = IHS_SessionSetStreamingClientConfig(s, bytes, len / 2, local != 0);
        else if (!strncmp(line, "NEGOTIATE", 9)) {
            CNegotiationInitMsg init = CNEGOTIATION_INIT_MSG__INIT;
            EStreamAudioCodec audio = k_EStreamAudioCodecOpus;
            EStreamVideoCodec video = k_EStreamVideoCodecH264;
            init.n_supported_audio_codecs = init.n_supported_video_codecs = 1;
            init.supported_audio_codecs = &audio;
            init.supported_video_codecs = &video;
            IHS_Buffer payload = IHS_BUFFER_INIT(128, 128);
            IHS_BufferAppendMessage(&payload, (const ProtobufCMessage *)&init);
            const IHS_SessionPacketHeader header = {0};
            IHS_SessionChannelControlOnNegotiation(s->channels[1], k_EStreamControlNegotiationInit,
                                                   &payload, &header);
            IHS_BufferClear(&payload, true);
        } else
            receive(s, bytes, len / 2);
        free(bytes);
        drain(s);
        if (emitting) {
            size_t n = IHS_SessionGetStreamingClientConfig(s, NULL, 0);
            uint8_t *wire = malloc(n + 1);
            assert(wire);
            assert(IHS_SessionGetStreamingClientConfig(s, wire, n) == n);
            fputs("],\"config\":", stdout);
            hex(wire, n);
            free(wire);
            printf(
                ",\"input_enabled\":%s,\"audio_enabled\":%s,\"video_enabled\":%s,\"result\":%s}\n",
                IHS_SessionInputEnabled(s) ? "true" : "false",
                s->state.streamingAudio ? "true" : "false",
                s->state.streamingVideo ? "true" : "false", result ? "true" : "false");
        }
    }
    free(line);
    IHS_SessionPacketsWindowDestroy(outgoing);
    IHS_SessionDestroy(s);
    IHS_Quit();
    return 0;
}
