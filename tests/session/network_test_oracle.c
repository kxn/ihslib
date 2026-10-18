/* Actual test-mode protocol/stats with identity cipher/final-admission adapters
 * matching the original instruction driver. No decoder or extra worker exists. */
#include "ihs_buffer.h"
#include "ihs_buffer_ext.h"
#include "ihslib.h"
#include "session/channels/ch_control.h"
#include "session/session_pri.h"
#include "test_session.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint32_t now;
static IHS_Session *session;
static char *frames[256];
static unsigned frameChannels[256], frameCount, controlId, dataId;
uint32_t __wrap_IHS_SessionPacketTimestamp(void) {
    return now;
}
bool __wrap_IHS_BaseSend(IHS_Base *s, IHS_SocketAddress a, const IHS_Buffer *b) {
    (void)s;
    (void)a;
    (void)b;
    return false;
}
int __wrap_IHS_SessionFrameEncrypt(IHS_Session *s, const uint8_t *in, size_t size, uint8_t *out,
                                   size_t *capacity, uint64_t sequence) {
    (void)s;
    (void)sequence;
    assert(*capacity >= size);
    memcpy(out, in, size);
    *capacity = size;
    return 0;
}
IHS_SessionFrameDecryptResult __wrap_IHS_SessionFrameDecrypt(IHS_Session *s, const IHS_Buffer *in,
                                                             IHS_Buffer *out, uint64_t expected,
                                                             uint64_t *actual) {
    (void)s;
    IHS_BufferAppendMem(out, IHS_BufferPointer(in), in->size);
    *actual = expected;
    return IHS_SessionFrameDecryptOK;
}
IHS_SendAdmission __wrap_IHS_SessionChannelQueueFrameResult(IHS_SessionChannel *ch,
                                                            IHS_SessionFrame *f, bool reliable) {
    (void)reliable;
    assert(frameCount < 256);
    frameChannels[frameCount] = ch->id;
    frames[frameCount] = malloc(f->body.size * 2 + 1);
    assert(frames[frameCount]);
    for (size_t i = 0; i < f->body.size; ++i)
        sprintf(frames[frameCount] + i * 2, "%02x", IHS_BufferPointer(&f->body)[i]);
    frames[frameCount][f->body.size * 2] = 0;
    ++frameCount;
    return IHS_SendAdmissionAccepted;
}
static void hex(const void *data, size_t size) {
    const unsigned char *p = data;
    for (size_t i = 0; i < size; ++i)
        printf("%02x", p[i]);
}
static void snapshot(const char *label) {
    IHS_NetworkTestResult result;
    IHS_SessionGetNetworkTestResult(session, &result);
    IHS_SessionNetworkTest *n = &session->networkTest;
    IHS_MediaNetworkStats *q = &session->mediaNetworkStats;
    IHS_SessionChannelControl *c = (void *)session->channels[1];
    assert(result.status == n->status && result.frameSpan == q->total &&
           result.samples == q->fullCount);
    printf("{\"label\":\"%s\",\"status\":%u,\"state\":%u,\"last_data\":%u,\"previous_kbps\":%u,"
           "\"current_kbps\":%u,\"tx_sequence\":%llu,\"rx_sequence\":%llu,\"total\":%u,\"batch\":%"
           "u,\"latencies\":[",
           label, n->status, n->phase, n->lastData, n->previousKbps, n->currentKbps,
           (unsigned long long)c->sendEncryptSequence, (unsigned long long)c->recvEncryptSequence,
           q->total, q->batch);
    for (size_t i = 0; i < q->count; ++i)
        printf("%s%u", i ? "," : "", q->latencies[i]);
    printf("],\"samples\":[");
    for (size_t i = 0; i < q->windowCount; ++i) {
        printf("%s\"", i ? "," : "");
        hex(&q->windows[i], 12);
        printf("\"");
    }
    printf("],\"average\":\"");
    hex(&q->average, 12);
    printf("\",\"full_count\":%zu,\"frame_count\":%u,\"frames\":[", q->fullCount, frameCount);
    for (unsigned i = 0; i < frameCount; ++i)
        printf("%s{\"channel\":%u,\"body\":\"%s\"}", i ? "," : "", frameChannels[i], frames[i]);
    printf("],\"full\":[");
    IHS_NetworkTestSample copy[256];
    size_t off = 0;
    for (;;) {
        size_t count = IHS_SessionReadNetworkTestSamples(session, off, copy, 256);
        if (!count)
            break;
        for (size_t i = 0; i < count; ++i)
            printf("%s[%u,%u]", off + i ? "," : "", copy[i].targetKbps, copy[i].networkTicks);
        off += count;
    }
    printf("],\"result\":%u}\n", result.result);
}
static void incoming(unsigned tag, const char *text, bool data) {
    IHS_SessionPacket packet = {
        .header = {.type = data ? IHS_SessionPacketTypeUnreliable : IHS_SessionPacketTypeReliable,
                   .channelId = data ? 3 : 1,
                   .srcConnectionId = 34,
                   .dstConnectionId = 12,
                   .packetId = data ? dataId++ : controlId++,
                   .sendTimestamp = now - 65}};
    IHS_SessionPacketBodyInitialize(&packet.body, false);
    IHS_BufferAppendUInt8(&packet.body, data ? k_EStreamDataPacket : tag);
    if (data) {
        IHS_BufferAppendUInt16LE(&packet.body, tag);
        IHS_BufferAppendUInt32LE(&packet.body, now - 66);
        IHS_BufferAppendUInt16LE(&packet.body, 0);
        IHS_BufferAppendUInt32LE(&packet.body, 0);
        const uint8_t header[7] = {0, 0, 0x18, 0, 0, 0, 0};
        IHS_BufferAppendMem(&packet.body, header, sizeof(header));
        for (unsigned i = 0; i < 80; ++i)
            IHS_BufferAppendUInt8(&packet.body, tag & 255u);
    } else if (strcmp(text, "-"))
        for (size_t i = 0; i < strlen(text); i += 2) {
            unsigned byte;
            assert(sscanf(text + i, "%2x", &byte) == 1);
            IHS_BufferAppendUInt8(&packet.body, byte);
        }
    IHS_SessionPacketPopulateBuffer(&packet);
    IHS_Buffer wire = IHS_BUFFER_INIT(2048, 2048);
    IHS_BufferAppendMem(&wire, packet.body.data, IHS_SessionPacketSize(&packet));
    session->base.callbacks.received(&session->base, &session->info.address, &wire);
    IHS_BufferClear(&wire, true);
    IHS_SessionPacketClear(&packet, true);
}
int main(int argc, char **argv) {
    assert(argc == 5);
    IHS_Init();
    session = IHS_TestSessionCreate();
    assert(session);
    IHS_TimerTaskStopImmediate(session->retransmission.timer);
    for (unsigned i = 0; i < 32; ++i)
        session->info.sessionKey[i] = i;
    now = strtoul(argv[3], NULL, 0);
    assert(IHS_SessionConfigureNetworkTest(session, atoi(argv[1]), strtoul(argv[2], NULL, 0)));
    /* HMAC16 is a primitive-only boundary; public API requires 32-byte tickets. */
    session->info.sessionKeyLen = strtoul(argv[4], NULL, 0);
    IHS_SessionNetworkTestBegin(session);
    session->state.connectionState = IHS_SessionConnectionStateConnecting;
    session->state.connectionId = 12;
    session->state.hostConnectionId = 34;
    IHS_SessionTransportBegin(session, now);
    IHS_SessionChannelControlHandshake(session->channels[1], true);
    IHS_SessionNetworkTestConnected(session, now);
    snapshot("started");
    char line[1024];
    unsigned index = 0;
    while (fgets(line, sizeof(line), stdin)) {
        unsigned tick, tag;
        char op, body[512] = "-";
        assert(sscanf(line, "%u %c %u %511s", &tick, &op, &tag, body) >= 2);
        now = tick;
        if (op == 'C')
            incoming(tag, body, false);
        else if (op == 'D')
            incoming(tag, body, true);
        else if (op == 'K')
            IHS_SessionDisconnect(session);
        else if (op == 'X')
            IHS_SessionClientTransportClosed(session);
        else
            assert(op == 'T');
        if (!session->base.interrupted && !session->transport.detached) {
            IHS_SessionNetworkUpdateAt(session, now);
            IHS_SessionTransportCheckTimeout(session, now);
        }
        char label[64];
        snprintf(label, sizeof(label), "after_network_%u", ++index);
        snapshot(label);
        if (!session->base.interrupted && !session->transport.detached)
            IHS_SessionNetworkTestUpdate(session, now);
    }
    if (session->networkTest.phase == 5 && !session->base.interrupted)
        IHS_SessionNetworkTestUpdate(session, now);
    IHS_SessionNetworkTestFinalize(session);
    snapshot("returned");
    for (unsigned i = 0; i < frameCount; ++i)
        free(frames[i]);
    IHS_SessionDestroy(session);
    IHS_Quit();
    return 0;
}
