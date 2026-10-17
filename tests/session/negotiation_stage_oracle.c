/* Actual encrypted wire ingress -> RX reliable drain -> client stage update ->
 * encrypted retained sender -> wire/decrypt. Clock/final UDP are controlled;
 * host ACKs retire observed output through the real session ingress. */
#include "ihs_buffer.h"
#include "ihs_buffer_ext.h"
#include "session/channels/ch_control.h"
#include "session/session_pri.h"
#include "test_session.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static IHS_Session *s;
static uint32_t now = 1000000;
static IHS_SessionPacketsWindow *output;
static uint64_t inputSequence, outputSequence;
static uint16_t inputId, ackId;
static unsigned frameCount, producedCount, connected;
static bool haveAck;
static char *frames[16], *produced[16];
static uint64_t producedSequence;
uint32_t __wrap_IHS_SessionPacketTimestamp(void) {
    return now;
}
static char *hex(const uint8_t *bytes, size_t size) {
    char *result = malloc(size * 2 + 1);
    assert(result);
    for (size_t i = 0; i < size; ++i)
        sprintf(result + i * 2, "%02x", bytes[i]);
    result[size * 2] = 0;
    return result;
}
static IHS_Buffer bytes(const char *text) {
    IHS_Buffer b = IHS_BUFFER_INIT(1024, 0);
    IHS_BufferEnsureCapacity(&b, 1);
    if (!strcmp(text, "-"))
        return b;
    for (size_t i = 0; text[i]; i += 2) {
        unsigned v;
        assert(text[i + 1] && sscanf(text + i, "%2x", &v) == 1);
        IHS_BufferAppendUInt8(&b, v);
    }
    return b;
}
IHS_SendAdmission __real_IHS_SessionChannelQueueFrameResult(IHS_SessionChannel *,
                                                            IHS_SessionFrame *, bool);
IHS_SendAdmission __wrap_IHS_SessionChannelQueueFrameResult(IHS_SessionChannel *ch,
                                                            IHS_SessionFrame *frame,
                                                            bool reliable) {
    /* Source's Connection::Send adapter observes this same producer boundary. */
    IHS_Buffer cipher = frame->body;
    uint8_t tag = *IHS_BufferPointer(&cipher);
    assert(tag == 5 || tag == 9);
    IHS_BufferOffsetBy(&cipher, 1);
    IHS_Buffer plain = IHS_BUFFER_INIT(1024, 0), full = IHS_BUFFER_INIT(1024, 0);
    uint64_t sequence;
    assert(IHS_SessionFrameDecrypt(s, &cipher, &plain, producedSequence++, &sequence) == 0);
    IHS_BufferAppendUInt8(&full, tag);
    IHS_BufferAppendMem(&full, IHS_BufferPointer(&plain), plain.size);
    assert(producedCount < 16);
    produced[producedCount++] = hex(IHS_BufferPointer(&full), full.size);
    IHS_BufferClear(&plain, true);
    IHS_BufferClear(&full, true);
    return __real_IHS_SessionChannelQueueFrameResult(ch, frame, reliable);
}
static void ingress(IHS_SessionPacket *p) {
    IHS_SessionPacketPopulateBuffer(p);
    IHS_Buffer wire = IHS_BUFFER_INIT(2048, 2048);
    IHS_BufferAppendMem(&wire, p->body.data, IHS_SessionPacketSize(p));
    s->base.callbacks.received(&s->base, &s->info.address, &wire);
    IHS_BufferClear(&wire, true);
}
bool __wrap_IHS_BaseSend(IHS_Base *base, IHS_SocketAddress address, const IHS_Buffer *wire) {
    (void)base;
    (void)address;
    IHS_Buffer copy = IHS_BUFFER_INIT(1024, 0);
    IHS_BufferAppendMem(&copy, IHS_BufferPointer(wire), wire->size);
    IHS_SessionPacket p;
    assert(IHS_SessionPacketParse(&p, &copy) == IHS_SessionPacketResultOK);
    if (p.header.type == 5 || p.header.type == 6) {
        assert(p.header.channelId == 1);
        ackId = p.header.packetId;
        haveAck = true;
        assert(IHS_SessionPacketsWindowAdd(output, &p));
        IHS_SessionFrame f = {.body = IHS_BUFFER_INIT(1024, 0)};
        while (IHS_SessionPacketsWindowPoll(output, &f)) {
            uint8_t tag = *IHS_BufferPointer(&f.body);
            assert(tag == 5 || tag == 9);
            IHS_BufferOffsetBy(&f.body, 1);
            IHS_Buffer plain = IHS_BUFFER_INIT(1024, 0), full = IHS_BUFFER_INIT(1024, 0);
            uint64_t sequence;
            assert(IHS_SessionFrameDecrypt(s, &f.body, &plain, outputSequence++, &sequence) == 0);
            IHS_BufferAppendUInt8(&full, tag);
            IHS_BufferAppendMem(&full, IHS_BufferPointer(&plain), plain.size);
            assert(frameCount < 16);
            frames[frameCount++] = hex(IHS_BufferPointer(&full), full.size);
            IHS_BufferClear(&plain, true);
            IHS_BufferClear(&full, true);
            IHS_SessionFrameClear(&f, true);
            f = (IHS_SessionFrame){.body = IHS_BUFFER_INIT(1024, 0)};
        }
        IHS_SessionFrameClear(&f, true);
    }
    IHS_SessionPacketClear(&p, true);
    IHS_BufferClear(&copy, true);
    return true;
}
static void received(const char *body) {
    IHS_Buffer plain = bytes(body);
    size_t capacity = plain.size + 64;
    uint8_t *cipher = malloc(capacity + 1);
    assert(cipher);
    cipher[0] = 4;
    assert(!IHS_SessionFrameEncrypt(s, IHS_BufferPointer(&plain), plain.size, cipher + 1, &capacity,
                                    inputSequence++));
    size_t total = capacity + 1, count = (total + 63) / 64;
    for (size_t i = 0, offset = 0; i < count; ++i) {
        IHS_SessionPacket p = {.header = {.type = i ? 6 : 5,
                                          .channelId = 1,
                                          .srcConnectionId = 34,
                                          .dstConnectionId = 12,
                                          .hasCrc = s->state.crcRequired,
                                          .packetId = inputId++,
                                          .fragmentId = i ? i - 1 : count - 1,
                                          .sendTimestamp = 900000}};
        IHS_SessionPacketBodyInitialize(&p.body, p.header.hasCrc);
        size_t size = total - offset;
        if (size > 64)
            size = 64;
        IHS_BufferAppendMem(&p.body, cipher + offset, size);
        ingress(&p);
        offset += size;
        IHS_SessionPacketClear(&p, true);
    }
    free(cipher);
    IHS_BufferClear(&plain, true);
}
static void onConnected(IHS_Session *session, void *ctx) {
    (void)session;
    (void)ctx;
    assert(s->state.connectionState == IHS_SessionConnectionStateConnected);
    assert(s->outgoing.priority[1] == 2 && s->outgoing.priority[2] == -1);
    ++connected;
}
static void report(void) {
    while (IHS_SessionDispatchQueued(s, false)) {
    }
    if (haveAck) {
        IHS_SessionPacket p = {.header = {.type = 7,
                                          .channelId = 1,
                                          .packetId = ackId,
                                          .srcConnectionId = 34,
                                          .dstConnectionId = 12,
                                          .hasCrc = s->state.crcRequired,
                                          .sendTimestamp = now}};
        IHS_SessionPacketBodyInitialize(&p.body, p.header.hasCrc);
        IHS_BufferAppendUInt32LE(&p.body, now);
        ingress(&p);
        IHS_SessionPacketClear(&p, true);
        haveAck = false;
    }
    CNegotiatedConfig empty = CNEGOTIATED_CONFIG__INIT;
    CNegotiatedConfig *cfg = s->negotiatedConfig ? s->negotiatedConfig : &empty;
    size_t n = cnegotiated_config__get_packed_size(cfg);
    uint8_t *p = malloc(n ? n : 1);
    assert(p);
    cnegotiated_config__pack(cfg, p);
    char *encoded = hex(p, n);
    free(p);
    printf("{\"state\":%d,\"config\":\"%s\",\"wire_frames\":[", (int)s->state.connectionState,
           encoded);
    free(encoded);
    for (unsigned i = 0; i < frameCount; ++i) {
        printf("%s\"%s\"", i ? "," : "", frames[i]);
        free(frames[i]);
    }
    frameCount = 0;
    printf("],\"frames\":[");
    for (unsigned i = 0; i < producedCount; ++i) {
        printf("%s\"%s\"", i ? "," : "", produced[i]);
        free(produced[i]);
    }
    producedCount = 0;
    n = IHS_SessionGetStreamingClientConfig(s, NULL, 0);
    p = malloc(n ? n : 1);
    assert(p);
    IHS_SessionGetStreamingClientConfig(s, p, n);
    encoded = hex(p, n);
    free(p);
    printf("],\"sequence\":%llu,\"keepalive_time\":%u,\"frame_events_time\":%u,\"client_config\":\"%s\","
           "\"connected\":%u,\"priorities\":[%d,%d]}\n",
           (unsigned long long)((IHS_SessionChannelControl *)s->channels[1])->sendEncryptSequence,
           ((IHS_SessionChannelControl *)s->channels[1])->keepAliveTimestamp,
           ((IHS_SessionChannelControl *)s->channels[1])->frameEventsTimestamp, encoded, connected,
           s->outgoing.priority[1], s->outgoing.priority[2]);
    free(encoded);
    connected = 0;
}
int main(int argc, char **argv) {
    assert(argc == 5);
    now = strtoul(argv[4], NULL, 0);
    IHS_Init();
    s = IHS_TestSessionCreate();
    IHS_TimerTaskStopImmediate(s->retransmission.timer);
    s->state.connectionState = atoi(argv[1]);
    s->state.connectionId = 12;
    s->state.hostConnectionId = 34;
    s->state.crcRequired = atoi(argv[2]);
    s->outgoing.rate[0] = s->outgoing.rate[1] = 0;
    IHS_Buffer initial = bytes(argv[3]);
    s->negotiatedConfig =
        cnegotiated_config__unpack(NULL, initial.size, IHS_BufferPointer(&initial));
    assert(s->negotiatedConfig);
    IHS_BufferClear(&initial, true);
    const IHS_StreamSessionCallbacks callbacks = {.connected = onConnected};
    IHS_SessionSetSessionCallbacks(s, &callbacks, NULL);
    output = IHS_SessionPacketsWindowCreateReliable(64, 0);
    char line[8192];
    while (fgets(line, sizeof(line), stdin)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "RX ", 3))
            received(line + 3);
        else if (!strcmp(line, "DRAIN")) {
            IHS_SessionReliableUpdate(s);
            IHS_SessionChannelControlDrainIncoming(s->channels[1]);
            report();
        } else {
            assert(sscanf(line, "UPDATE %u", &now) == 1);
            s->base.callbacks.run->update(&s->base, s->base.callbackContexts.run);
            report();
        }
    }
    IHS_SessionPacketsWindowDestroy(output);
    IHS_SessionDestroy(s);
    IHS_Quit();
    return 0;
}
