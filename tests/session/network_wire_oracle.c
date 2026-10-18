/* Actual CRC parse -> session receive admission and actual packet send ->
 * controlled final I/O, with live session collectors. RX body consumers may
 * run, but their final sends are rejected/excluded like source adapters. */
#include "ihslib.h"
#include "endianness.h"
#include "session/session_pri.h"
#include "session/channels/ch_control.h"
#include "session/frame_stats.h"
#include "session/channels/ch_data.h"
#include "session/reliable.h"
#include "session/channels/video/ch_data_video.h"
#include "test_session.h"
#include "media_network_snapshot.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static IHS_Session *session;
static uint32_t now;
static bool receiving, sending;
static unsigned resultCode, entryCount, writeCount, entries[512];
static bool reportMetadata, lossMode, videoMode, cycleMode, networkMode, deadlineMode, admissionMode, earlyMode;
static unsigned admitted[256], admittedCount;
static bool earlyClosing, clockMode, qualityMode;
static char *acceptedClock[32], *decodedClock[32];
static unsigned acceptedClockCount, decodedClockCount;
static IHS_SessionChannelData mediaStorage, *media = &mediaStorage;
static unsigned deliveredCount;
static char *delivered[256];
static IHS_SessionPacketHeader deliveredMetadata[256];
static bool initialize_media(IHS_SessionChannel *ch, uint16_t capacity) {
    IHS_SessionChannelData *d = (void *)ch;
    d->receive = IHS_SessionUnreliableGet(ch->session, ch->id, capacity);
    if (d->receive) d->receive->maxReceiveDelay = 6553;
    d->fecQueueCapacity = capacity;
    d->fecQueueLimit = (size_t)capacity * 2048;
    d->windowLock = IHS_MutexCreate();
    d->windowCond = IHS_CondCreate();
    atomic_init(&d->interrupted, false);
    return d->receive && d->windowLock && d->windowCond;
}
bool __real_IHS_SessionChannelDataInit(IHS_SessionChannel *, uint16_t);
bool __wrap_IHS_SessionChannelDataInit(IHS_SessionChannel *ch, uint16_t capacity) {
    /* Only replace worker creation/waits: real VideoInit, receive consumer,
     * collector, codec-header admission and destruction still run. */
    if (videoMode) return initialize_media(ch, capacity);
    return __real_IHS_SessionChannelDataInit(ch, capacity);
}
static char *clock_metadata(IHS_SessionFrame *f) {
    IHS_Buffer body = f->body;
    IHS_BufferOffsetBy(&body, 1);
    IHS_SessionDataFrameHeader h = {0};
    assert(IHS_SessionChannelDataFrameHeaderParse(&h, &body) == 12);
    int32_t offset = f->mediaPrepared ? f->mediaOffset : IHS_StreamClockOffset(&session->clock);
    char value[512];
    snprintf(value, sizeof(value), "{\"id\":%u,\"frame\":%u,\"sent\":%u,\"received\":%u,\"first\":%u,\"input\":%u,\"input_received\":%u,\"size\":%zu}",
        h.id, h.timestamp - offset, f->header.sendTimestamp - offset, f->header.receiveTimestamp,
        f->header.receiveBegin, h.inputMark, h.inputRecvTimestamp ? h.inputRecvTimestamp - offset : 0, f->body.size - 13);
    return strdup(value);
}
void __real_IHS_SessionChannelDataReceiveReliableFrame(IHS_SessionChannel *, IHS_SessionFrame *);
void __wrap_IHS_SessionChannelDataReceiveReliableFrame(IHS_SessionChannel *ch, IHS_SessionFrame *f) {
    if (clockMode) { assert(acceptedClockCount < 32); acceptedClock[acceptedClockCount++] = clock_metadata(f); }
    __real_IHS_SessionChannelDataReceiveReliableFrame(ch, f);
}
static void deliver(IHS_SessionFrame *f) {
    if (clockMode) { assert(decodedClockCount < 32); decodedClock[decodedClockCount++] = clock_metadata(f); }
    assert(deliveredCount < 256);
    char *hex = malloc(f->body.size * 2 + 1);
    assert(hex);
    hex[0] = 0;
    for (size_t i = 0; i < f->body.size; ++i)
        sprintf(hex + i * 2, "%02x", IHS_BufferPointer(&f->body)[i]);
    deliveredMetadata[deliveredCount] = f->header;
    delivered[deliveredCount++] = hex;
    if (admissionMode && f->body.size >= 3 && *IHS_BufferPointer(&f->body) == k_EStreamDataPacket) {
        assert(admittedCount < 256);
        uint16_t id;
        IHS_ReadUInt16LE(IHS_BufferPointerAt(&f->body, 1), &id);
        admitted[admittedCount++] = id;
    }
    if (videoMode) IHS_SessionChannelDataReceiveFrame(media, f);
}
static void drain_media(void) {
    if (!media) return;
    for (;;) {
        IHS_SessionFrame frame = {0}; uint16_t missing;
        IHS_MutexLock(media->windowLock);
        bool ready = IHS_SessionChannelDataPollLocked(media, &frame, &missing);
        IHS_MutexUnlock(media->windowLock);
        assert(!missing);
        if (ready) deliver(&frame);
        IHS_SessionFrameClear(&frame, true);
        if (!ready) break;
    }
}
IHS_ReliableDisposition __real_IHS_SessionDeliverReliableFrame(IHS_Session *, IHS_SessionFrame *);
IHS_ReliableDisposition __wrap_IHS_SessionDeliverReliableFrame(IHS_Session *s, IHS_SessionFrame *f) {
    if (lossMode && (!videoMode || !f->body.size || *IHS_BufferPointer(&f->body) != k_EStreamDataPacket)) {
        /* Fixed non-data FEC vectors stop at the same transport-delegate
         * adapter as Source; they do not exercise unknown-message dispatch. */
        deliver(f); return IHS_ReliableData;
    }
    return __real_IHS_SessionDeliverReliableFrame(s, f);
}
static const IHS_SessionChannelClass mediaClass = {.received = IHS_SessionChannelDataReceived};
static struct { IHS_Buffer wire; unsigned cost, work; } cycleRows[256];
static unsigned cycleCount, cycleIndex, cycleEOF, cycleWork, writeCost;
static bool cycleReady;
static unsigned waits[8], waitCount;
static char *cycleWires[256];
static unsigned cycleWireCount;
static char *initialWires[256];
static unsigned initialWireCount;
static struct { unsigned kind, channel, id; } txSeeds[256];
static unsigned txSeedCount;
static struct { unsigned state, local, peer, tick; } notices[4];
static unsigned noticeCount;
uint8_t __wrap_IHS_SessionNextConnectionId(void) { return 12; }
void __real_IHS_SessionClientTransportClosed(IHS_Session *);
void __wrap_IHS_SessionClientTransportClosed(IHS_Session *s) {
    assert(noticeCount < 4);
    notices[noticeCount].state=IHS_SessionTransportState(s);
    notices[noticeCount].local=s->state.connectionId;
    notices[noticeCount].peer=s->state.hostConnectionId;
    notices[noticeCount++].tick=now;
    __real_IHS_SessionClientTransportClosed(s);
}
static void save_initial_wires(void) {
    for (unsigned i=0;i<cycleWireCount;++i) { assert(initialWireCount < 256); initialWires[initialWireCount++]=cycleWires[i]; }
    cycleWireCount=writeCount=entryCount=0;
}

static void report_tx(void) {
    printf(",\"tx\":{\"windows\":[");
    for (unsigned i = 0; i < txSeedCount; ++i) {
        unsigned kind = txSeeds[i].kind, ch = txSeeds[i].channel, id = txSeeds[i].id;
        IHS_OutgoingWindow *w = &session->outgoing.unreliable[ch];
        IHS_OutgoingPacket *p = kind == 3 && w->capacity &&
            (uint16_t)(id-w->head) < (uint16_t)(w->tail-w->head) ?
            w->slots[id & (w->capacity-1)] : NULL;
        bool retained = kind == 3 ? p != NULL :
            IHS_RetransmissionIsTracked(&session->retransmission, ch, id, 0);
        printf("%s{\"kind\":%u,\"channel\":%u,\"id\":%u,\"retained\":%s", i ? "," : "", kind, ch, id, retained ? "true" : "false");
        if (kind == 3)
            printf(",\"head\":%u,\"tail\":%u,\"cursor\":%u,\"count\":%u,\"stamp\":%u", w->head, w->tail, w->cursor,
                p ? p->packet.header.retransmitCount : 0, p ? p->packet.header.sendTimestamp : 0);
        printf("}");
    }
    printf("]");
    if (!deadlineMode) {
        printf(",\"order\":[");
        for (unsigned i=0; i<session->outgoing.count; ++i)
            printf("%s%u", i ? "," : "", session->outgoing.order[i]);
        printf("]");
    }
    printf(",\"rotation\":%u,\"blocked\":%s,\"deadlines\":[%u,%u]},\"initial_wires\":[", session->outgoing.rotation,
        session->outgoing.blocked ? "true" : "false", session->outgoing.deadline[0], session->outgoing.deadline[1]);
    for (unsigned i=0; i<initialWireCount; ++i) {
        printf("%s\"%s\"", i ? "," : "", initialWires[i]); free(initialWires[i]);
    }
    initialWireCount=0;
    printf("]");
}
int __real_IHS_UDPSocketWaitReadable(IHS_UDPSocket *, uint32_t);
int __wrap_IHS_UDPSocketWaitReadable(IHS_UDPSocket *socket, uint32_t milliseconds) {
    if (!cycleMode) return __real_IHS_UDPSocketWaitReadable(socket, milliseconds);
    waits[waitCount++] = milliseconds; return cycleReady;
}
int __real_IHS_UDPSocketReceive(IHS_UDPSocket *, IHS_UDPPacket *);
int __wrap_IHS_UDPSocketReceive(IHS_UDPSocket *socket, IHS_UDPPacket *packet) {
    if (!cycleMode) return __real_IHS_UDPSocketReceive(socket, packet);
    if (cycleIndex == cycleCount) { now += cycleEOF; return 0; }
    unsigned i = cycleIndex++;
    now += cycleRows[i].cost; cycleWork = cycleRows[i].work;
    packet->address = session->info.address;
    IHS_BufferAppend(&packet->buffer, &cycleRows[i].wire);
    return 1;
}
static unsigned frameCount;
static uint64_t producedSequence;
static struct { unsigned channel; char *body; } frames[8];
static struct { unsigned size, wireSize; bool crc; } writes[256];
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
    if (receiving && cycleMode) now += cycleWork;
    if (receiving && result == IHS_SessionPacketResultOK) {
        assert(entryCount < 512);
        entries[entryCount++] = IHS_PACKET_HEADER_SIZE + packet->body.size;
    }
    return result;
}
bool __wrap_IHS_BaseSend(IHS_Base *base, IHS_SocketAddress address, const IHS_Buffer *wire) {
    (void)base; (void)address;
    if (!sending && !cycleMode && !admissionMode) { errno = EAGAIN; return false; }
    assert(writeCount < 256);
    if (cycleMode || admissionMode) {
        assert(cycleWireCount < 256);
        char *hex = malloc(wire->size * 2 + 1); assert(hex); hex[0] = 0;
        for (size_t i = 0; i < wire->size; ++i)
            sprintf(hex + i * 2, "%02x", IHS_BufferPointer(wire)[i]);
        cycleWires[cycleWireCount++] = hex;
    }
    bool crc = (*IHS_BufferPointer(wire) & 128) != 0;
    writes[writeCount].wireSize = wire->size;
    writes[writeCount].size = wire->size - (crc ? 4 : 0);
    writes[writeCount++].crc = crc;
    static const int errors[] = {0, EAGAIN, EMSGSIZE, EBADF, EIO};
    now += writeCost;
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
    if (lossMode) {
        printf(",\"delivered\":[");
        for (unsigned i = 0; i < deliveredCount; ++i) {
            printf("%s\"%s\"", i ? "," : "", delivered[i]);
            free(delivered[i]);
        }
        printf("]");
        if (videoMode || cycleMode) {
            printf(",\"metadata\":[");
            for (unsigned i = 0; i < deliveredCount; ++i) {
                const IHS_SessionPacketHeader *h = &deliveredMetadata[i];
                printf("%s{\"first\":%u,\"last\":%u,\"sent\":%u,\"fragment\":%u}",
                    i ? "," : "", h->receiveBegin, h->receiveTimestamp, h->sendTimestamp,
                    (uint16_t)h->fragmentId);
            }
            printf("]");
        }
        if (videoMode) {
            IHS_NetworkVideoStats *v = &session->networkVideo;
            printf(",\"video\":{\"frame\":%u,\"pending\":[%u,%u,%u],\"totals\":[%u,%u,%u,%u],\"bits\":[%u,%u],\"samples\":",
                v->frame, v->firstSent, v->lastReceived, v->frameBytes,
                v->totals[0], v->totals[1], v->totals[2], v->totals[3], bits(v->bitrate), bits(v->link));
            samples(&v->queue, 4); printf("}");
        }
        deliveredCount = 0;
    }
    if (admissionMode) {
        printf(",\"client\":%u,\"client_paused\":%s,\"admitted\":[", session->state.connectionState,
            session->transport.clientPaused ? "true" : "false");
        for (unsigned i = 0; i < admittedCount; ++i) printf("%s%u", i ? "," : "", admitted[i]);
        printf("],\"wires\":[");
        for (unsigned i = 0; i < cycleWireCount; ++i) { printf("%s\"%s\"", i ? "," : "", cycleWires[i]); free(cycleWires[i]); }
        printf("]"); admittedCount = cycleWireCount = 0;
    }
    if (earlyMode) {
        if (earlyClosing) printf(",\"connection\":%u", IHS_SessionTransportState(session));
        printf(",\"pending\":[");
        struct Prefix { IHS_SessionPacket packet; };
        unsigned count = session->pendingDataCount;
        for (unsigned i = 0; i < count; ++i) {
            IHS_QueueItem *item = IHS_QueuePoll(session->pendingData); assert(item);
            IHS_SessionPacket *p = &((struct Prefix *)item)->packet;
            printf("%s{\"kind\":%u,\"channel\":%u,\"fragment\":%u,\"id\":%u,\"sent\":%u,\"first\":%u,\"received\":%u,\"body\":\"", i ? "," : "",
                p->header.type | (p->header.hasCrc ? 128 : 0), p->header.channelId, p->header.fragmentId,
                p->header.packetId, p->header.sendTimestamp, p->header.receiveBegin, p->header.receiveTimestamp);
            for (size_t j = 0; j < p->body.size; ++j) printf("%02x", IHS_BufferPointer(&p->body)[j]);
            printf("\"}"); IHS_QueueAppend(session->pendingData, item);
        }
        IHS_UnreliableChannel *rx = session->unreliable[3]; assert(rx);
        unsigned head = IHS_SessionPacketsWindowNextNeededPacketId(rx->window);
        printf("],\"receiver\":{\"head\":%u,\"tail\":%u,\"delay\":%u}", head,
            (uint16_t)(head + IHS_SessionPacketsWindowSize(rx->window)), rx->maxReceiveDelay);
    }
    if (clockMode) {
        printf(",\"offset\":%d,\"accepted\":[", IHS_StreamClockOffset(&session->clock));
        for (unsigned i = 0; i < acceptedClockCount; ++i) { printf("%s%s", i ? "," : "", acceptedClock[i]); free(acceptedClock[i]); }
        printf("],\"decoded\":[");
        for (unsigned i = 0; i < decodedClockCount; ++i) { printf("%s%s", i ? "," : "", decodedClock[i]); free(decodedClock[i]); }
        printf("]"); acceptedClockCount = decodedClockCount = 0;
    }
    if (cycleMode) {
        printf(",\"waits\":[");
        for (unsigned i = 0; i < waitCount; ++i) printf("%s%u", i ? "," : "", waits[i]);
        printf("],\"wires\":[");
        for (unsigned i = 0; i < cycleWireCount; ++i) {
            printf("%s\"%s\"", i ? "," : "", cycleWires[i]); free(cycleWires[i]);
        }
        cycleWireCount = waitCount = 0;
        printf("],\"now\":%u,\"update_tick\":%u,\"last_receive\":%u,\"ping\":{\"sum\":%u,\"bits\":%u,\"samples\":",
            now, session->transport.updateTime, session->transport.lastReceive,
            session->clock.ping.sum, bits(session->clock.ping.milliseconds));
        samples(&session->clock.ping.queue, 1);
        printf("},\"clock\":{\"echo\":%u,\"minimum\":%u,\"smooth\":%u,\"next_min_update\":%u,\"offset\":%u}",
            session->clock.lastEcho, session->clock.minRTT, session->clock.smoothedRTT,
            session->clock.minRTTUpdate, (uint32_t)session->clock.offset);
    }
    if (networkMode) report_tx();
    if (deadlineMode) {
        printf(",\"lifecycle\":{\"state\":%u,\"local\":%u,\"peer\":%u,\"notices\":[", IHS_SessionTransportState(session),
            (unsigned)session->state.connectionId,(unsigned)session->state.hostConnectionId);
        for (unsigned i=0;i<noticeCount;++i)
            printf("%s{\"state\":%u,\"local\":%u,\"peer\":%u,\"tick\":%u}", i ? "," : "", notices[i].state,notices[i].local,notices[i].peer,notices[i].tick);
        noticeCount=0; printf("]}");
    }
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
    if (qualityMode) { printf(",\"quality\":"); quality_report(session); }
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
    assert(argc >= 3 && argc <= 6);
    clockMode = argc == 6 && !strcmp(argv[5], "media-clock");
    earlyClosing = argc == 6 && !strcmp(argv[5], "early-closing");
    earlyMode = clockMode || earlyClosing || (argc == 6 && !strcmp(argv[5], "early"));
    admissionMode = earlyMode || (argc == 6 && !strcmp(argv[5], "admission"));
    qualityMode = argc == 6 && !strcmp(argv[5], "quality");
    videoMode = qualityMode || admissionMode || (argc == 6 && !strcmp(argv[5], "video"));
    deadlineMode = argc == 6 && !strcmp(argv[5], "deadline");
    networkMode = deadlineMode || (argc == 6 && !strcmp(argv[5], "network"));
    cycleMode = networkMode || (argc == 6 && !strcmp(argv[5], "cycle"));
    lossMode = videoMode || cycleMode || (argc == 6 && !strcmp(argv[5], "loss"));
    IHS_Init();
    session = IHS_TestSessionCreate();
    IHS_TimerTaskStopImmediate(session->retransmission.timer);
    session->state.connectionState = cycleMode ? IHS_SessionConnectionStateHandshaking : IHS_SessionConnectionStateConnected;
    session->state.connectionId = 12;
    session->state.hostConnectionId = argc >= 5 ? atoi(argv[4]) : 34;
    session->state.crcRequired = atoi(argv[1]);
    reportMetadata = argc >= 4 && atoi(argv[3]) >= 0;
    if (reportMetadata) IHS_FrameStatsAggregatorSetFullReporting(session->frameStats, atoi(argv[3]));
    session->outgoing.rate[0] = session->outgoing.rate[1] = 0;
    if (lossMode) {
        if (earlyMode) {
            media = NULL;
            IHS_OutgoingRegister(session, 3);
            assert(IHS_SessionUnreliableGet(session, 3, 2048));
        } else if (videoMode) {
            CStartVideoDataMsg config = CSTART_VIDEO_DATA_MSG__INIT;
            config.channel = 3;
            config.codec = k_EStreamVideoCodecH264;
            config.width = 1280; config.height = 720;
            media = (void *)IHS_SessionChannelDataVideoCreate(session, &config);
            assert(media && !media->base.initFailed);
        } else {
            media->base = (IHS_SessionChannel){.cls = &mediaClass, .id = 3, .session = session,
                                              .type = IHS_SessionChannelTypeDataVideo};
            assert(initialize_media(&media->base, 1024));
        }
        if (media) IHS_SessionChannelAdd(session, &media->base);
    }
    bool initializedCycle = false;
    char line[8192];
    while (fgets(line, sizeof(line), stdin)) {
        unsigned type, ch, crc, source, dest, size, bad;
        if (qualityMode && (line[0] == 'Z' || line[0] == 'E')) {
            if (line[0] == 'Z') IHS_MediaNetworkStatsStart(&session->mediaNetworkStats);
            else assert(IHS_MediaNetworkStatsStop(&session->mediaNetworkStats));
            report(); continue;
        }
        if (clockMode && line[0] == 'V') {
            unsigned id;
            assert(sscanf(line, "V %u %u", &id, &now) == 2);
            IHS_FrameStatsRecordCompleteAt(session->frameStats, id, IHS_VideoFrameResultDisplayed, now);
            report(); continue;
        }
        if (clockMode && line[0] == 'D') { drain_media(); report(); continue; }
        if (earlyMode && line[0] == 'X') {
            IHS_SessionPacket p = packet(IHS_SessionPacketTypeDisconnect, 0, session->state.crcRequired, 34, 12, 0);
            IHS_SessionPacketPopulateBuffer(&p);
            IHS_Buffer wire = IHS_BUFFER_INIT(2048, 2048);
            IHS_BufferAppendMem(&wire, p.body.data, IHS_SessionPacketSize(&p));
            receiving = true;
            session->base.callbacks.received(&session->base, &session->info.address, &wire);
            receiving = false;
            IHS_BufferClear(&wire, true); IHS_SessionPacketClear(&p, true);
            report(); continue;
        }
        if (earlyMode && line[0] == 'C') {
            unsigned id;
            assert(sscanf(line, "C %u", &id) == 1);
            if (media) IHS_SessionChannelRemove(session, media->base.id);
            media = NULL;
            assert(IHS_SessionUnreliableSetDelay(session, id, 6553));
            CStartVideoDataMsg config = CSTART_VIDEO_DATA_MSG__INIT;
            config.channel = id; config.codec = k_EStreamVideoCodecH264; config.width = 1280; config.height = 720;
            media = (void *)IHS_SessionChannelDataVideoCreate(session, &config); assert(media);
            IHS_SessionChannelAdd(session, &media->base);
            if (!clockMode) drain_media(); report(); continue;
        }
        if (deadlineMode && line[0] == 'L') {
            unsigned last,paused;
            assert(sscanf(line,"L %u %u",&last,&paused)==2 && paused<2);
            session->transport.active=true;
            session->transport.lastReceive=last;
            session->transport.clientPaused=session->transport.connectionPaused=paused;
            session->transport.appliedGeneration=paused;
            atomic_store(&session->transport.lifecycleGeneration,paused);
            continue;
        }
        if (deadlineMode && line[0] == 'D') {
            IHS_SessionTransportDisconnect(session); save_initial_wires(); continue;
        }
        if (networkMode && line[0] == 'I') {
            unsigned cls, rate, deadline;
            assert(sscanf(line, "I %u %u %u", &cls, &rate, &deadline) == 3 && cls < 2);
            session->outgoing.rate[cls] = (int32_t)rate;
            session->outgoing.deadline[cls] = deadline;
            continue;
        }
        if (networkMode && line[0] == 'A') {
            char hex[4096];
            assert(sscanf(line, "A %u %u %u %4095s %u", &now, &type, &ch, hex, &resultCode) == 5);
            assert(type == 3 || type == 5);
            IHS_OutgoingRegister(session, ch);
            IHS_SessionPacket p = packet(type, ch, session->state.crcRequired, 12, 34, strlen(hex)/2);
            p.header.autoPacketId = true;
            for (unsigned i=0; i<p.body.size; ++i) { unsigned v; assert(sscanf(hex+i*2,"%2x",&v)==1); IHS_BufferPointer(&p.body)[i]=v; }
            assert(IHS_SessionQueuePacket(session, &p, type == 5));
            assert(txSeedCount < 256);
            txSeeds[txSeedCount].kind=type; txSeeds[txSeedCount].channel=ch; txSeeds[txSeedCount++].id=p.header.packetId;
            assert(IHS_SessionDispatchQueued(session, false));
            IHS_SessionPacketClear(&p, true);
            save_initial_wires();
            continue;
        }
        if (line[0] == 'Q') {
            if (admissionMode) {
                unsigned phase, paused;
                assert(sscanf(line, "Q %u %u", &phase, &paused) == 2);
                session->state.connectionState = phase;
                session->transport.clientPaused = paused;
                report();
                continue;
            }
            int priority;
            assert(cycleMode && sscanf(line, "Q %u %d", &ch, &priority) == 2 && ch < 32);
            IHS_OutgoingPriority(session, ch, priority);
            continue;
        }
        if (line[0] == 'B') {
            unsigned ready, blocked;
            assert(cycleMode && sscanf(line, "B %u %u %u %u %u", &now, &ready, &blocked,
                &cycleEOF, &cycleCount) == 5 && cycleCount <= 256);
            if (networkMode) assert(sscanf(line, "B %*u %*u %*u %*u %*u %u %u", &resultCode, &writeCost) == 2);
            cycleReady = ready; cycleIndex = 0;
            session->outgoing.blocked = blocked;
            if (!initializedCycle) {
                initializedCycle = true;
                if (!deadlineMode) session->transport.lastReceive = now;
                session->clock.lastEcho = now; session->clock.haveEcho = true;
            }
            for (unsigned n = 0; n < cycleCount; ++n) {
                unsigned id, retry, fragment, sent; char hex[4096];
                assert(fgets(line, sizeof(line), stdin));
                assert(sscanf(line, "P %u %u %u %u %u %u %4095s %u %u", &type, &ch,
                    &id, &retry, &fragment, &sent, hex, &cycleRows[n].cost, &cycleRows[n].work) == 9);
                size = strlen(hex)/2;
                IHS_SessionPacket p = packet(type, ch, session->state.crcRequired, 34, 12, size);
                p.header.packetId=id; p.header.fragmentId=fragment; p.header.retransmitCount=retry; p.header.sendTimestamp=sent;
                for (unsigned i=0;i<size;++i) { unsigned v; assert(sscanf(hex+i*2,"%2x",&v)==1); IHS_BufferPointer(&p.body)[i]=v; }
                IHS_SessionPacketPopulateBuffer(&p);
                IHS_BufferInit(&cycleRows[n].wire, 2048, 2048);
                IHS_BufferAppendMem(&cycleRows[n].wire,p.body.data,IHS_SessionPacketSize(&p));
                IHS_SessionPacketClear(&p,true);
            }
            IHS_UDPPacket rx = {.buffer = IHS_BUFFER_INIT(2048,2048)};
            receiving = true;
            bool keepRunning=session->base.callbacks.run->receiveCycle(&session->base, NULL, &rx);
            assert(deadlineMode || keepRunning);
            receiving = false;
            for (;;) {
                IHS_SessionFrame frame = {0}; uint16_t missing;
                IHS_MutexLock(media->windowLock);
                bool readyFrame = IHS_SessionChannelDataPollLocked(media, &frame, &missing);
                IHS_MutexUnlock(media->windowLock);
                assert(missing == 0);
                if (readyFrame) deliver(&frame);
                IHS_SessionFrameClear(&frame, true);
                if (!readyFrame) break;
            }
            IHS_BufferClear(&rx.buffer, true);
            for (unsigned n=0;n<cycleCount;++n) IHS_BufferClear(&cycleRows[n].wire,true);
        } else if (line[0] == 'W') {
            unsigned id, retry, fragment, sent;
            char hex[4096];
            assert(lossMode);
            assert(sscanf(line, "W %u %u %u %u %u %u %u %4095s", &now, &type, &ch,
                          &id, &retry, &fragment, &sent, hex) == 8);
            size = strlen(hex) / 2;
            IHS_SessionPacket p = packet(type, ch, session->state.crcRequired, 34, 12, size);
            p.header.packetId = id;
            p.header.retransmitCount = retry;
            p.header.fragmentId = fragment;
            p.header.sendTimestamp = sent;
            for (unsigned i = 0; i < size; ++i) {
                unsigned value;
                assert(sscanf(hex + i * 2, "%2x", &value) == 1);
                IHS_BufferPointer(&p.body)[i] = value;
            }
            IHS_SessionPacketPopulateBuffer(&p);
            IHS_Buffer wire = IHS_BUFFER_INIT(2048, 2048);
            IHS_BufferAppendMem(&wire, p.body.data, IHS_SessionPacketSize(&p));
            receiving = true;
            session->base.callbacks.received(&session->base, &session->info.address, &wire);
            receiving = false;
            if (type == IHS_SessionPacketTypeUnreliableFEC && media && !clockMode) {
                /* Source delegates FEC synchronously. Native bridges through
                 * its worker FIFO; drain that production step within this
                 * action to compare payload/counters, not callback timing. */
                IHS_SessionFrame f = {.body = IHS_BUFFER_INIT(1024, 0)};
                uint16_t missing;
                IHS_MutexLock(media->windowLock);
                bool ready = IHS_SessionChannelDataPollLocked(media, &f, &missing);
                IHS_MutexUnlock(media->windowLock);
                assert(missing == 0);
                if (ready) deliver(&f);
                IHS_SessionFrameClear(&f, true);
            }
            IHS_BufferClear(&wire, true);
            IHS_SessionPacketClear(&p, true);
        } else if (line[0] == 'U') {
            assert(lossMode && sscanf(line, "U %u %u", &now, &ch) == 2);
            if (videoMode || ch != 3) IHS_SessionReliableUpdateChannel(session, ch);
            if (ch == 3 && earlyMode) {
                IHS_SessionUnreliableUpdateAt(session, ch, now);
                if (!clockMode) drain_media();
            } else if (ch == 3) {
                IHS_SessionChannelDataUpdateAt(media, now);
                /* Drive the production complete-frame FIFO consumer; actual
                 * decoder scheduling remains outside this oracle. */
                for (;;) {
                    IHS_SessionFrame frame = {.body = IHS_BUFFER_INIT(1024, 0)};
                    uint16_t missing;
                    IHS_MutexLock(media->windowLock);
                    bool ready = IHS_SessionChannelDataPollLocked(media, &frame, &missing);
                    IHS_MutexUnlock(media->windowLock);
                    IHS_SessionRecordNetworkLost(session, now, missing);
                    if (ready) deliver(&frame);
                    IHS_SessionFrameClear(&frame, true);
                    if (!ready && !missing) break;
                }
            }
        } else if (!strncmp(line, "RX ", 3)) {
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
        } else if (line[0] == 'O') {
            unsigned offset;
            assert(videoMode && sscanf(line, "O %u", &offset) == 1);
            IHS_MutexLock(session->clock.lock);
            session->clock.offset = (int32_t)offset;
            IHS_MutexUnlock(session->clock.lock);
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
    if (lossMode && media) {
        assert(session->channels[session->numChannels - 1] == &media->base);
        session->channels[--session->numChannels] = NULL;
        assert(media->fecQueuedCount == 0);
        if (videoMode) {
            IHS_SessionChannelDestroy(&media->base);
        } else {
        IHS_MutexDestroy(media->windowLock);
        IHS_CondDestroy(media->windowCond);
        }
    }
    IHS_SessionDestroy(session);
    IHS_Quit();
    return 0;
}
