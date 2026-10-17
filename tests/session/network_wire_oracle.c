/* Actual CRC parse -> session receive admission and actual packet send ->
 * controlled final I/O, with live session collectors. RX body consumers may
 * run, but their final sends are rejected/excluded like source adapters. */
#include "ihslib.h"
#include "session/session_pri.h"
#include "session/channels/ch_control.h"
#include "session/frame_stats.h"
#include "test_session.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static IHS_Session *session;
static uint32_t now;
static bool receiving, sending;
static unsigned resultCode, entryCount, writeCount, entries[8];
static bool reportMetadata;
static unsigned frameCount;
static uint64_t producedSequence;
static struct { unsigned channel; char *body; } frames[8];
static struct { unsigned size, wireSize; bool crc; } writes[8];
IHS_SendAdmission __real_IHS_SessionChannelQueueFrameResult(IHS_SessionChannel *, IHS_SessionFrame *, bool);
IHS_SendAdmission __wrap_IHS_SessionChannelQueueFrameResult(IHS_SessionChannel *ch,
                                                            IHS_SessionFrame *frame, bool reliable) {
    assert(frameCount < 8);
    IHS_Buffer body = frame->body, plain = IHS_BUFFER_INIT(1024, 0);
    uint8_t tag = *IHS_BufferPointer(&body);
    IHS_BufferOffsetBy(&body, 1);
    if (ch->id == 1) {
        uint64_t sequence;
        assert(!IHS_SessionFrameDecrypt(session, &body, &plain, producedSequence++, &sequence));
        body = plain;
    }
    frames[frameCount].channel = ch->id;
    frames[frameCount].body = malloc(body.size * 2 + 3);
    assert(frames[frameCount].body);
    sprintf(frames[frameCount].body, "%02x", tag);
    for (size_t i = 0; i < body.size; ++i)
        sprintf(frames[frameCount].body + 2 + i * 2, "%02x", IHS_BufferPointer(&body)[i]);
    ++frameCount;
    IHS_BufferClear(&plain, true);
    return __real_IHS_SessionChannelQueueFrameResult(ch, frame, reliable);
}
uint32_t __wrap_IHS_SessionPacketTimestamp(void) { return now; }
IHS_SessionPacketReturn __real_IHS_SessionPacketParse(IHS_SessionPacket *, IHS_Buffer *);
IHS_SessionPacketReturn __wrap_IHS_SessionPacketParse(IHS_SessionPacket *packet, IHS_Buffer *wire) {
    IHS_SessionPacketReturn result = __real_IHS_SessionPacketParse(packet, wire);
    if (receiving && result == IHS_SessionPacketResultOK) {
        assert(entryCount < 8);
        entries[entryCount++] = IHS_PACKET_HEADER_SIZE + packet->body.size;
    }
    return result;
}
bool __wrap_IHS_BaseSend(IHS_Base *base, IHS_SocketAddress address, const IHS_Buffer *wire) {
    (void)base; (void)address;
    if (!sending) { errno = EAGAIN; return false; }
    assert(writeCount < 8);
    bool crc = (*IHS_BufferPointer(wire) & 128) != 0;
    writes[writeCount].wireSize = wire->size;
    writes[writeCount].size = wire->size - (crc ? 4 : 0);
    writes[writeCount++].crc = crc;
    static const int errors[] = {0, EAGAIN, EMSGSIZE, EBADF, EIO};
    errno = errors[resultCode];
    return resultCode == 0;
}
static unsigned bits(float value) {
    unsigned result; memcpy(&result, &value, sizeof(result)); return result;
}
static void word(uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) printf("%02x", (value >> (8 * i)) & 255);
}
static void samples(const IHS_NetworkSampleQueue *q, unsigned values) {
    printf("[");
    for (size_t i = 0; i < q->count; ++i) {
        const IHS_NetworkSample *sample = &q->data[(q->head + i) & (q->capacity - 1)];
        printf("%s\"", i ? "," : ""); word(sample->timestamp);
        for (unsigned j = 0; j < values; ++j) word(sample->values[j]);
        printf("\"");
    }
    printf("]");
}
static void report(void) {
    printf("{\"rates\":[");
    for (unsigned i = 0; i < 2; ++i) {
        IHS_NetworkBitrateStats *s = &session->networkRates[i];
        printf("%s{\"sum\":%llu,\"bits\":%u,\"samples\":", i ? "," : "",
               (unsigned long long)s->bytes, bits(s->bitrate));
        samples(&s->queue, 1); printf("}");
    }
    IHS_NetworkPacketStats *p = &session->networkPackets;
    printf("],\"packets\":{\"total\":%u,\"lost\":%u,\"samples\":", p->total, p->lost);
    samples(&p->queue, 2);
    printf("},\"entries\":[");
    for (unsigned i = 0; i < entryCount; ++i) printf("%s%u", i ? "," : "", entries[i]);
    printf("],\"writes\":[");
    for (unsigned i = 0; i < writeCount; ++i)
        printf("%s{\"size\":%u,\"wire_size\":%u,\"crc\":%s}", i ? "," : "",
               writes[i].size, writes[i].wireSize, writes[i].crc ? "true" : "false");
    printf("]");
    if (reportMetadata) {
        printf(",\"frames\":[");
        for (unsigned i = 0; i < frameCount; ++i)
            printf("%s{\"channel\":%u,\"body\":\"%s\"}", i ? "," : "", frames[i].channel, frames[i].body);
        IHS_SessionChannelControl *control = (void *)session->channels[1];
        printf("],\"frame_events_time\":%u,\"keepalive_time\":%u,\"sequence\":%llu,\"reported\":%u",
               control->frameEventsTimestamp, control->keepAliveTimestamp,
               (unsigned long long)control->sendEncryptSequence, session->frameStats->lastSentFrameId);
    }
    for (unsigned i = 0; i < frameCount; ++i) free(frames[i].body);
    frameCount = 0;
    printf("}\n");
    entryCount = writeCount = 0;
}
static IHS_SessionPacket packet(unsigned type, unsigned channel, unsigned crc,
                                  unsigned source, unsigned dest, unsigned size) {
    IHS_SessionPacket p = {.header = {.type = type, .channelId = channel, .hasCrc = crc,
                                     .srcConnectionId = source, .dstConnectionId = dest,
                                     .sendTimestamp = now}};
    IHS_SessionPacketBodyInitialize(&p.body, crc);
    IHS_BufferFillMem(&p.body, 0, 0, size);
    return p;
}
int main(int argc, char **argv) {
    assert(argc >= 3 && argc <= 5);
    IHS_Init();
    session = IHS_TestSessionCreate();
    IHS_TimerTaskStopImmediate(session->retransmission.timer);
    session->state.connectionState = IHS_SessionConnectionStateConnected;
    session->state.connectionId = 12;
    session->state.hostConnectionId = argc == 5 ? atoi(argv[4]) : 34;
    session->state.crcRequired = atoi(argv[1]);
    reportMetadata = argc >= 4 && atoi(argv[3]) >= 0;
    if (reportMetadata) IHS_FrameStatsAggregatorSetFullReporting(session->frameStats, atoi(argv[3]));
    session->outgoing.rate[0] = session->outgoing.rate[1] = 0;
    char line[256];
    while (fgets(line, sizeof(line), stdin)) {
        unsigned type, ch, crc, source, dest, size, bad;
        if (!strncmp(line, "RX ", 3)) {
            assert(sscanf(line, "RX %u %u %u %u %u %u %u %u", &now, &type, &ch, &crc,
                          &source, &dest, &size, &bad) == 8);
            IHS_SessionPacket p = packet(type, ch, crc, source, dest, size);
            IHS_SessionPacketPopulateBuffer(&p);
            IHS_Buffer wire = IHS_BUFFER_INIT(2048, 2048);
            IHS_BufferAppendMem(&wire, p.body.data, IHS_SessionPacketSize(&p));
            if (bad) *IHS_BufferPointerAt(&wire, wire.size - 1) ^= 1;
            IHS_SocketAddress address = session->info.address;
            if (!atoi(argv[2])) ++address.port;
            receiving = true;
            session->base.callbacks.received(&session->base, &address, &wire);
            receiving = false;
            IHS_BufferClear(&wire, true);
            IHS_SessionPacketClear(&p, true);
        } else if (!strncmp(line, "TX ", 3)) {
            assert(sscanf(line, "TX %u %u %u %u %u", &now, &type, &ch, &size, &resultCode) == 5);
            IHS_SessionPacket p = packet(type, ch, 0, 12, 34, size);
            sending = true;
            IHS_SessionSendPacketResult(session, &p);
            sending = false;
            IHS_SessionPacketClear(&p, true);
        } else if (line[0] == 'R') {
            unsigned id;
            assert(sscanf(line, "R %u", &id) == 1);
            IHS_FrameStatsRecordReceived(session->frameStats, id, 900000, 901000, 902000, 100, 0);
        } else if (line[0] == 'C') {
            unsigned id;
            assert(sscanf(line, "C %u %u", &id, &now) == 2);
            IHS_FrameStatsRecordCompleteAt(session->frameStats, id, IHS_VideoFrameResultDisplayed, now);
        } else if (line[0] == 'T') {
            unsigned echo, peer;
            assert(sscanf(line, "T %u %u %u", &echo, &peer, &now) == 3);
            IHS_StreamClockFeedback(&session->clock, echo, peer, now);
        } else {
            assert(sscanf(line, "S %u", &now) == 1);
            IHS_SessionChannelControlUpdateStage(session->channels[1], now);
        }
        report();
    }
    IHS_SessionDestroy(session);
    IHS_Quit();
    return 0;
}
