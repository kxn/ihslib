/* Independent wire oracles from Android 1.3.32 ARM64; see protocol RE §14. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "common/test_session.h"
#include "ihs_buffer_ext.h"
#include "session/channels/ch_control.h"
#include "session/frame_stats.h"
#include "session/channels/ch_data.h"

struct IHS_QueueItem { IHS_SessionPacket packet; bool reliable; };
static IHS_Session *session_new(void) {
    IHS_Session *s = IHS_TestSessionCreate();
    IHS_TimerTaskStopImmediate(s->retransmission.timer);
    IHS_SessionChannelControl *c = (void *) s->channels[1];
    IHS_TimerTaskStopImmediate(c->feedbackTimer);
    return s;
}
static void init(IHS_SessionPacket *p, unsigned type, unsigned id, int fragment) {
    memset(p, 0, sizeof(*p));
    p->header.type = type; p->header.channelId = 1;
    p->header.packetId = id; p->header.fragmentId = fragment;
    p->header.sendTimestamp = 123456;
    IHS_SessionPacketBodyInitialize(&p->body, false);
}
static void receive(IHS_Session *s, unsigned type, unsigned id, int frag) {
    IHS_SessionPacket p;
    init(&p, type, id, frag);
    IHS_BufferAppendUInt8(&p.body, k_EStreamControlClientHandshake);
    s->channels[1]->cls->received(s->channels[1], &p);
    IHS_SessionPacketClear(&p, true);
}
static void feedback(IHS_Session *s, int confirmed, int base, int mask) {
    bool ack = false, nack = false;
    IHS_QueueItem *q;
    while ((q = IHS_QueuePoll(s->sendQueue))) {
        const uint8_t *b = IHS_BufferPointer(&q->packet.body);
        if (q->packet.header.type == IHS_SessionPacketTypeACK) {
            assert(q->packet.header.packetId == (uint16_t) confirmed);
            uint32_t echo = b[0] | (uint32_t)b[1]<<8 | (uint32_t)b[2]<<16 | (uint32_t)b[3]<<24;
            assert(echo >= 123456 && echo < 124112); /* echoed peer clock, <=10 ms processing */
            ack = true;
        } else if (q->packet.header.type == IHS_SessionPacketTypeNACK) {
            assert(base >= 0);
            assert(q->packet.header.packetId == (uint16_t) base);
            assert((b[4] | b[5]<<8) == (uint16_t) confirmed);
            assert(b[6] == mask);
            nack = true;
        }
        IHS_SessionPacketClear(&q->packet, true); IHS_QueueItemFree(q);
    }
    assert(ack);
    assert(nack == (base >= 0));
}
static void gaps_and_wrap(void) {
    IHS_Session *s = session_new();
    IHS_SessionChannelControl *c = (void *) s->channels[1];
    /* Lost first packet must remain a hole, not establish a new sequence origin. */
    receive(s, IHS_SessionPacketTypeReliable, 1, 0);
    feedback(s, 65535, 0, 2);
    receive(s, IHS_SessionPacketTypeReliable, 0, 0);
    feedback(s, 1, -1, 0);
    receive(s, IHS_SessionPacketTypeReliable, 1, 0); /* lost ACK -> duplicate */
    feedback(s, 1, -1, 0);
    IHS_SessionPacketsWindowDestroy(c->framePacketWindow);
    c->framePacketWindow = IHS_SessionPacketsWindowCreateReliable(320, 65535);
    c->lastNackSentMs = 0;
    receive(s, IHS_SessionPacketTypeReliable, 65535, 2); /* head needs two fragments */
    feedback(s, 65535, -1, 0);
    receive(s, IHS_SessionPacketTypeReliableFrag, 1, 1); /* missing continuation 0 */
    feedback(s, 65535, 0, 2);
    receive(s, IHS_SessionPacketTypeReliableFrag, 0, 0);
    feedback(s, 1, -1, 0);
    IHS_SessionDestroy(s);
}
static void reliable_window_growth(void) {
    IHS_SessionPacketsWindow *w = IHS_SessionPacketsWindowCreateReliable(320, 65535);
    IHS_SessionPacket p;
    init(&p, IHS_SessionPacketTypeReliable, 1023, 0); /* 1024 ahead of first ID */
    IHS_BufferAppendUInt8(&p.body, 6);
    assert(IHS_SessionPacketsWindowAdd(w, &p));
    assert(IHS_SessionPacketsWindowSize(w) == 1025);
    assert(IHS_SessionPacketsWindowContiguousId(w) == 65534);
    IHS_SessionPacketClear(&p, true);
    init(&p, IHS_SessionPacketTypeReliable, 65535, 0);
    IHS_BufferAppendUInt8(&p.body, 6);
    assert(IHS_SessionPacketsWindowAdd(w, &p));
    assert(IHS_SessionPacketsWindowContiguousId(w) == 65535);
    IHS_SessionPacketClear(&p, true);
    init(&p, IHS_SessionPacketTypeReliable, 16383, 0); /* distance 0x4000: ignored */
    IHS_BufferAppendUInt8(&p.body, 6);
    assert(IHS_SessionPacketsWindowAdd(w, &p));
    assert(IHS_SessionPacketsWindowSize(w) == 1025);
    IHS_SessionPacketClear(&p, true);
    IHS_SessionPacketsWindowDestroy(w);
}
static unsigned drain_nacks(IHS_Session *s) {
    unsigned count = 0;
    IHS_MutexLock(s->sendQueueMutex);
    IHS_QueueItem *q;
    while ((q = IHS_QueuePoll(s->sendQueue))) {
        if (q->packet.header.type == IHS_SessionPacketTypeNACK) {
            assert(q->packet.header.packetId == 0);
            count++;
        }
        IHS_SessionPacketClear(&q->packet, true);
        IHS_QueueItemFree(q);
    }
    IHS_MutexUnlock(s->sendQueueMutex);
    return count;
}
static void lost_nack_recovery(void) {
    IHS_Session *s = IHS_TestSessionCreate(); /* keep feedback timer running */
    receive(s, IHS_SessionPacketTypeReliable, 1, 0);
    assert(drain_nacks(s) >= 1); /* pretend this feedback was lost */
    unsigned repeated = 0;
    uint64_t deadline = IHS_TimerNow() + 1000;
    while (repeated < 2 && IHS_TimerNow() < deadline) {
        usleep(5000);
        repeated += drain_nacks(s);
    }
    assert(repeated >= 2); /* recovery must not require another incoming packet */
    receive(s, IHS_SessionPacketTypeReliable, 0, 0);
    drain_nacks(s);
    usleep(20000);
    assert(drain_nacks(s) == 0);
    IHS_SessionDestroy(s);
}
static void nack_layout(void) {
    IHS_Session *s = session_new();
    for (unsigned i = 0; i < 4; i++) {
        IHS_SessionPacket p;
        init(&p, IHS_SessionPacketTypeReliable, (uint16_t)(65534 + i), i);
        IHS_BufferAppendUInt8(&p.body, 6);
        assert(IHS_RetransmissionTrack(&s->retransmission, &p, 100));
        IHS_SessionPacketClear(&p, true);
    }
    IHS_SessionPacket p;
    init(&p, IHS_SessionPacketTypeNACK, 65535, 0);
    IHS_BufferAppendUInt32LE(&p.body, 200); /* not a sequence ID */
    IHS_BufferAppendUInt16LE(&p.body, 65534);
    IHS_BufferAppendUInt8(&p.body, 2); /* selectively confirms fragment id 2, packet 0 */
    s->channels[1]->cls->received(s->channels[1], &p);
    assert(s->retransmission.stats.outstanding == 2);
    assert(IHS_RetransmissionIsTracked(&s->retransmission, 1, 65535, 1));
    assert(IHS_RetransmissionIsTracked(&s->retransmission, 1, 1, 3));
    /* Stale NACK must not clear the missing packet. */
    p.header.sendTimestamp--;
    IHS_BufferPointer(&p.body)[6] = 0xff;
    s->channels[1]->cls->received(s->channels[1], &p);
    assert(s->retransmission.stats.outstanding == 2);
    IHS_SessionPacketClear(&p, true);
    init(&p, IHS_SessionPacketTypeNACK, 2, 0);
    IHS_BufferAppendUInt8(&p.body, 0); /* malformed extension */
    uint64_t nacks = s->retransmission.stats.nacks;
    s->channels[1]->cls->received(s->channels[1], &p);
    assert(s->retransmission.stats.nacks == nacks);
    IHS_SessionPacketClear(&p, true);
    IHS_SessionDestroy(s);
}
static bool retry_ten(IHS_SessionPacket *p, void *context) {
    (void)context;
    assert(p->header.packetId == 10);
    return true;
}
static void nack_age_and_trailing_zeros(void) {
    IHS_Session *s = session_new();
    uint32_t now = IHS_SessionPacketTimestamp();
    IHS_StreamClockFeedback(&s->clock, now-65536, now, now); /* 50ms NACK age ceiling */
    assert(IHS_StreamClockNackAgeTicks(&s->clock) == 3276);
    uint64_t later = IHS_TimerNow() + 100000; /* isolate NACK from ordinary retry deadlines */
    for (unsigned id = 10; id <= 12; id++) {
        IHS_SessionPacket p;
        init(&p, IHS_SessionPacketTypeReliable, id, 0);
        IHS_BufferAppendUInt8(&p.body, 6);
        assert(IHS_RetransmissionTrack(&s->retransmission, &p, later));
        p.header.sendTimestamp = IHS_SessionPacketTimestamp();
        IHS_RetransmissionNoteInitialSend(&s->retransmission, &p.header, true, later);
        IHS_SessionPacketClear(&p, true);
    }
    IHS_SessionPacket p;
    init(&p, IHS_SessionPacketTypeNACK, 10, 0);
    IHS_BufferAppendUInt32LE(&p.body, IHS_SessionPacketTimestamp());
    IHS_BufferAppendUInt16LE(&p.body, 9);
    IHS_BufferAppendUInt8(&p.body, 2); /* 10 missing, 11 held; no assertion about 12 */
    s->channels[1]->cls->received(s->channels[1], &p);
    assert(s->retransmission.stats.nacks == 1);
    assert(s->retransmission.stats.outstanding == 2);
    assert(IHS_RetransmissionProcessAt(&s->retransmission, IHS_TimerNow(), retry_ten, NULL) == 1);
    IHS_SessionPacketClear(&p, true);
    IHS_SessionDestroy(s);
}
static void decrypt_attempts(void) {
    IHS_Session *s = session_new();
    IHS_SessionChannelControl *c = (void *) s->channels[1];
    /* Malformed ciphertext consumes one attempted encrypted frame, no crash. */
    IHS_SessionPacket p;
    init(&p, IHS_SessionPacketTypeReliable, 0, 0);
    IHS_BufferAppendUInt8(&p.body, k_EStreamControlSetInputTemporarilyDisabled);
    c->base.cls->received(&c->base, &p);
    assert(c->recvEncryptSequence == 1);
    IHS_SessionPacketClear(&p, true);
    uint8_t msg[] = {0x08, 0x01}; /* disabled=true */
    uint8_t cipher[64]; size_t len = sizeof(cipher);
    assert(IHS_SessionFrameEncrypt(s, msg, sizeof(msg), cipher, &len, 1) == 0);
    init(&p, IHS_SessionPacketTypeReliable, 1, 0);
    IHS_BufferAppendUInt8(&p.body, k_EStreamControlSetInputTemporarilyDisabled);
    IHS_BufferAppendMem(&p.body, cipher, len);
    c->base.cls->received(&c->base, &p);
    assert(c->recvEncryptSequence == 2);
    assert(s->state.inputTemporarilyDisabled);
    assert(IHS_SessionInputEnabled(s)); /* Informational dialog, not HID admission. */
    IHS_SessionPacketClear(&p, true);
    IHS_SessionDestroy(s);
}
static void clock_and_events(void) {
    IHS_StreamClock c;
    IHS_StreamClockInit(&c);
    assert(IHS_StreamClockRetryTicks(&c) == 81);
    assert(IHS_StreamClockNackAgeTicks(&c) == 65);
    IHS_StreamClockFeedback(&c, 1000, 2100, 1200); /* RTT 200, offset 1000 */
    assert(IHS_StreamClockOffset(&c) == 1000);
    assert(IHS_StreamClockRetryTicks(&c) == 250);
    assert(IHS_StreamClockNackAgeTicks(&c) == 196);
    IHS_StreamClockFeedback(&c, 2000, 3102, 2200);
    assert(IHS_StreamClockOffset(&c) == 1001);
    IHS_StreamClockFeedback(&c, 3000, 999999, 5000); /* congested, excluded */
    assert(IHS_StreamClockOffset(&c) == 1001);
    IHS_StreamClockFeedback(&c, 700000, 702100, 700200); /* old samples expire */
    assert(IHS_StreamClockOffset(&c) == 2000);
    IHS_StreamClockDeinit(&c);
    IHS_StreamClockInit(&c);
    IHS_StreamClockFeedback(&c, UINT32_MAX-99, 1050, 0); /* clock wrap, offset 1100 */
    assert(IHS_StreamClockOffset(&c) == 1100);
    IHS_StreamClockDeinit(&c);
    IHS_FrameStatsSlot slot = {.eventMask = (1u<<0)|(1u<<5)|(1u<<13)|(1u<<15)|(1u<<18)};
    slot.events[0] = 1000; slot.events[5] = 900; slot.events[13] = 1100;
    slot.events[15] = 1125; slot.events[18] = 1150;
    CFrameEvent rows[19], *ptrs[19];
    assert(IHS_FrameStatsEncodeEvents(&slot, 50, rows, ptrs) == 4);
    assert(rows[0].timestamp == 1050 && rows[1].timestamp == 100);
    assert(rows[2].timestamp == 25 && rows[3].timestamp == 25);
    assert(rows[1].event_id == 13); /* omitted host-only events */
    slot.eventMask = (1u<<13)|(1u<<18);
    slot.events[13] = UINT32_MAX-9; slot.events[18] = 10;
    assert(IHS_FrameStatsEncodeEvents(&slot, 20, rows, ptrs) == 2);
    assert(rows[0].timestamp == 10 && rows[1].timestamp == 20);
}
static void deliver_wire(IHS_Session *s, IHS_SessionPacket *p) {
    IHS_SessionPacketPopulateBuffer(p);
    IHS_Buffer wire = IHS_BUFFER_INIT(2048, 2048);
    IHS_BufferAppendMem(&wire, p->body.data, IHS_SessionPacketSize(p));
    s->base.callbacks.received(&s->base, &s->info.address, &wire);
    IHS_BufferClear(&wire, true);
}
static unsigned earlyPackets;
static void handshake_unconnected_probe(void) {
    IHS_Session *s = session_new();
    s->state.connectionState = IHS_SessionConnectionStateConnecting;
    s->state.connectionId = 0xa3;
    IHS_SessionPacket p;
    init(&p, IHS_SessionPacketTypeConnectACK, 0, 0);
    p.header.channelId = 0;
    p.header.srcConnectionId = 0xd5;
    p.header.dstConnectionId = 0xa3;
    IHS_BufferAppendUInt32LE(&p.body, 0);
    deliver_wire(s, &p);
    IHS_SessionPacketClear(&p, true);
    assert(s->state.connectionState == IHS_SessionConnectionStateHandshaking);
    IHS_QueueItem *q = IHS_QueuePoll(s->sendQueue);
    assert(q && q->packet.header.channelId == 1);
    assert(*IHS_BufferPointer(&q->packet.body) == k_EStreamControlClientHandshake);
    IHS_SessionPacketClear(&q->packet, true); IHS_QueueItemFree(q);

    /* good.pcapng frame 43: MTU probe arrives AFTER Connected, with both IDs zero.
     * CStreamSocket::HandleMessage 0x7ff660 routes type 0 before connection checks. */
    static const uint8_t request[] = {1, 4, 0, 0, 0, 8, 2, 16, 0};
    init(&p, IHS_SessionPacketTypeUnconnected, 0, 0);
    p.header.channelId = 0;
    IHS_BufferAppendMem(&p.body, request, sizeof(request));
    deliver_wire(s, &p);
    IHS_SessionPacketClear(&p, true);
    q = IHS_QueuePoll(s->sendQueue);
    assert(q && q->packet.header.type == IHS_SessionPacketTypeUnconnected);
    const uint8_t *body = IHS_BufferPointer(&q->packet.body);
    assert(body[0] == k_EStreamDiscoveryPingResponse);
    CDiscoveryPingResponse *response = cdiscovery_ping_response__unpack(NULL,
        q->packet.body.size - 5, body + 5);
    assert(response && response->sequence == 2);
    assert(response->packet_size_received == IHS_PACKET_HEADER_SIZE + sizeof(request));
    cdiscovery_ping_response__free_unpacked(response, NULL);
    IHS_SessionPacketClear(&q->packet, true); IHS_QueueItemFree(q);
    IHS_SessionDestroy(s);
}
static void receive_early(IHS_SessionChannel *channel, IHS_SessionPacket *p) {
    assert(channel->id == 4 && p->header.packetId == 77);
    assert(p->header.receiveTimestamp != 0);
    assert(p->body.size == 1 && *IHS_BufferPointer(&p->body) == 99);
    earlyPackets++;
}
static void session_feedback_and_early_data(void) {
    IHS_Session *s = session_new();
    s->state.connectionState = IHS_SessionConnectionStateConnected;
    s->state.connectionId = 12; s->state.hostConnectionId = 34;
    IHS_SessionPacket p;
    init(&p, IHS_SessionPacketTypeReliable, 10, 0);
    p.header.hidReport = true;
    IHS_BufferAppendUInt8(&p.body, 6);
    assert(IHS_RetransmissionTrack(&s->retransmission, &p, 100));
    IHS_SessionReliabilityStats stats;
    IHS_SessionGetReliabilityStats(s, &stats);
    assert(stats.hidPending == 1 && stats.hidInFlight == 0 && stats.hidAcknowledged == 0);
    IHS_RetransmissionNoteInitialSend(&s->retransmission, &p.header, true, 101);
    IHS_SessionGetReliabilityStats(s, &stats);
    assert(stats.hidPending == 0 && stats.hidInFlight == 1);
    IHS_SessionPacketClear(&p, true);
    init(&p, IHS_SessionPacketTypeACK, 10, 0);
    IHS_BufferAppendUInt32LE(&p.body, IHS_SessionPacketTimestamp());
    p.header.srcConnectionId = 34; p.header.dstConnectionId = 11;
    deliver_wire(s, &p); /* old destination session */
    assert(s->retransmission.stats.outstanding == 1);
    p.header.dstConnectionId = 12; p.header.srcConnectionId = 33;
    deliver_wire(s, &p); /* old peer session */
    assert(s->retransmission.stats.outstanding == 1);
    p.header.srcConnectionId = 34;
    deliver_wire(s, &p);
    IHS_SessionGetReliabilityStats(s, &stats);
    assert(stats.hidInFlight == 0 && stats.hidAcknowledged == 1);
    assert(stats.reliableMaxAckLatencyMs > 0);
    deliver_wire(s, &p); /* duplicate cannot inflate HID ack count */
    IHS_SessionGetReliabilityStats(s, &stats);
    assert(stats.hidAcknowledged == 1);
    IHS_SessionPacketClear(&p, true);
    init(&p, IHS_SessionPacketTypeUnreliable, 77, 0);
    p.header.channelId = 4; p.header.srcConnectionId = 34; p.header.dstConnectionId = 12;
    IHS_BufferAppendUInt8(&p.body, 99);
    deliver_wire(s, &p);
    assert(IHS_SessionChannelFor(s, 4) == NULL && s->pendingDataCount == 1);
    static const IHS_SessionChannelClass cls = {.received=receive_early, .instanceSize=sizeof(IHS_SessionChannel)};
    IHS_SessionChannel *ch = IHS_SessionChannelCreate(&cls, s, IHS_SessionChannelTypeDataVideo, 4, NULL);
    IHS_SessionChannelAdd(s, ch);
    assert(earlyPackets == 1 && s->pendingDataCount == 0);
    IHS_SessionPacketClear(&p, true);
    IHS_SessionDestroy(s);
}
static void diagnostic_and_header_bounds(void) {
    char out[4096];
    while (IHS_SessionChannelControlDrainPendingHIDReports(out, sizeof(out))) {}
    IHS_Session *s = session_new();
    uint8_t bytes[512]; memset(bytes, 0x5a, sizeof(bytes));
    assert(IHS_SessionChannelControlSubmitHIDReport(s->channels[1], bytes, sizeof(bytes), true));
    char small[2] = {'x', 'y'};
    assert(IHS_SessionChannelControlDrainPendingHIDReports(small, 1) == 0);
    assert(small[1] == 'y'); /* no dequeue and no write beyond cap */
    assert(IHS_SessionChannelControlDrainPendingHIDReports(out, sizeof(out)) > 1024);
    assert(strstr(out, "5a5a5a5a"));
    assert(IHS_SessionChannelControlDrainPendingHIDReports(out, sizeof(out)) == 0);
    for (unsigned i = 0; i < 130; i++) {
        bytes[0] = i;
        assert(IHS_SessionChannelControlSubmitHIDReport(s->channels[1], bytes, 1, true));
    }
    size_t off = 0; uint64_t ms; uint16_t len; uint8_t recent[96];
    for (unsigned i = 0; i < 128; i++) {
        IHS_SessionChannelControlGetRecentHIDReports(&ms, &len, recent, &off);
        assert(len == 1 && recent[0] == 129-i);
    }
    IHS_SessionDataFrameHeader header;
    IHS_Buffer b = IHS_BUFFER_INIT(16, 16);
    for (unsigned i = 0; i < 12; i++) {
        assert(IHS_SessionChannelDataFrameHeaderParse(&header, &b) == 0);
        IHS_BufferAppendUInt8(&b, 0);
    }
    assert(IHS_SessionChannelDataFrameHeaderParse(&header, &b) == 12);
    IHS_BufferClear(&b, true);
    IHS_SessionDestroy(s);
}
static void negotiation_capabilities(void) {
    IHS_Session *s = session_new();
    s->state.connectionState = IHS_SessionConnectionStateNegotiating;
    CNegotiationInitMsg message = CNEGOTIATION_INIT_MSG__INIT;
    message.has_reliable_data = true; message.reliable_data = true;
    message.has_supports_remote_hid = true; message.supports_remote_hid = true;
    EStreamVideoCodec video = k_EStreamVideoCodecH264;
    message.n_supported_video_codecs = 1; message.supported_video_codecs = &video;
    IHS_Buffer body = IHS_BUFFER_INIT(1024, 1024);
    IHS_BufferAppendMessage(&body, (ProtobufCMessage *)&message);
    IHS_SessionPacketHeader header = {0};
    IHS_SessionChannelControlOnNegotiation(s->channels[1], k_EStreamControlNegotiationInit, &body, &header);
    IHS_QueueItem *q = IHS_QueuePoll(s->sendQueue);
    assert(q && q->packet.header.fragmentId == 0);
    assert(*IHS_BufferPointer(&q->packet.body) == k_EStreamControlNegotiationSetConfig);
    IHS_BufferOffsetBy(&q->packet.body, 1);
    IHS_Buffer plain = IHS_BUFFER_INIT(2048, 2048);
    uint64_t sequence;
    assert(IHS_SessionFrameDecrypt(s, &q->packet.body, &plain, 0, &sequence) == 0);
    CNegotiationSetConfigMsg *config = cnegotiation_set_config_msg__unpack(NULL, plain.size, IHS_BufferPointer(&plain));
    assert(config && config->config);
    assert(config->config->has_reliable_data && !config->config->reliable_data);
    assert(config->config->enable_remote_hid && !config->config->enable_touch_input);
    assert(config->config->selected_video_codec == k_EStreamVideoCodecH264);
    cnegotiation_set_config_msg__free_unpacked(config, NULL);
    IHS_BufferClear(&plain, true); IHS_BufferClear(&body, true);
    IHS_SessionPacketClear(&q->packet, true); IHS_QueueItemFree(q);
    IHS_SessionDestroy(s);
}
static void audio_reconfiguration(void) {
    IHS_Session *s = session_new();
    CStartAudioDataMsg message = CSTART_AUDIO_DATA_MSG__INIT;
    message.channel = 3;
    message.has_codec = true; message.codec = k_EStreamAudioCodecOpus;
    for (unsigned i = 0; i < 32; i++) {
        IHS_Buffer body = IHS_BUFFER_INIT(64, 64);
        message.channel = i % 2 ? 3 : 5;
        IHS_BufferAppendMessage(&body, (ProtobufCMessage *)&message);
        IHS_SessionChannelControlOnAudio(s->channels[1], k_EStreamControlStartAudioData, &body, NULL);
        IHS_SessionChannel *audio = IHS_SessionChannelForType(s, IHS_SessionChannelTypeDataAudio);
        assert(audio && audio->id == message.channel && s->numChannels == 4);
        IHS_BufferClear(&body, true);
    }
    IHS_SessionDestroy(s);
}
int main(void) {
    IHS_Init();
    gaps_and_wrap(); nack_layout(); decrypt_attempts(); clock_and_events();
    reliable_window_growth();
    lost_nack_recovery();
    nack_age_and_trailing_zeros();
    handshake_unconnected_probe();
    session_feedback_and_early_data();
    diagnostic_and_header_bounds();
    negotiation_capabilities(); audio_reconfiguration();
    IHS_Quit();
    puts("assembly-derived protocol regressions OK");
}
