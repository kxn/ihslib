/* Original-client vectors drive the actual production consumer, assembly,
 * tracker and reset actor. The idle worker, backend/player and clock are
 * explicit boundaries. Q supplies a pending request at that declared boundary. */
#include "../../src/session/channels/video/ch_data_video.c"
#include "ihs_buffer_ext.h"
#include "test_session.h"
#include "decoder_direct_consumer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef IHS_VIDEO_FLOW_CAPACITY
#define IHS_VIDEO_FLOW_CAPACITY 96
#endif
#ifndef IHS_VIDEO_HELD_CAPACITY
#define IHS_VIDEO_HELD_CAPACITY 32
#endif
static bool initialBackend, ready, haveBackend, recording;
static unsigned starts, action, flowCount, heldCount;
static char *flow[IHS_VIDEO_FLOW_CAPACITY];
static IHS_FrameTicket *held[IHS_VIDEO_HELD_CAPACITY];
static uint16_t pictures[IHS_VIDEO_HELD_CAPACITY];
static IHS_SessionChannelVideo *video;
static FILE *event(void) {
    assert(flowCount < IHS_VIDEO_FLOW_CAPACITY);
    size_t size;
    FILE *f = open_memstream(&flow[flowCount++], &size);
    assert(f);
    return f;
}
static void simple(const char *name) {
    if (!recording) return;
    FILE *f = event();
    fprintf(f, "[\"%s\",%u]", name, action);
    assert(!fclose(f));
}
uint32_t __wrap_IHS_SessionPacketTimestamp(void) { return 1000000; }
bool __wrap_IHS_SessionSendControlMessage(IHS_Session *s, EStreamControlMessage t,
                                          const ProtobufCMessage *m) {
    (void)s; (void)m;
    if (t == k_EStreamControlVideoDecoderInfo) simple("info");
#ifdef IHS_SUBFRAME_OBSERVER
    if (t == k_EStreamControlVideoOverflow && recording) {
        uint8_t bytes[32];
        size_t size = protobuf_c_message_get_packed_size(m);
        assert(size <= sizeof(bytes) && protobuf_c_message_pack(m, bytes) == size);
        FILE *f = event();
        fprintf(f, "[\"overflow\",%u,\"", action);
        for (size_t n = 0; n < size; ++n) fprintf(f, "%02x", bytes[n]);
        fprintf(f, "\"]");
        assert(!fclose(f));
    }
#endif
    return true;
}
void __wrap_IHS_SessionChannelDataLost(IHS_SessionChannel *c) {
    (void)c; simple("lost");
}
static int prepare(IHS_Session *s, const IHS_StreamVideoConfig *c, void *x) {
    (void)s; (void)c; (void)x; return 0;
}
static int start(IHS_Session *s, const IHS_VideoEpochInfo *e, const IHS_StreamVideoConfig *c,
                 void *x) {
    (void)s; (void)e; (void)c; (void)x;
    simple("start");
    haveBackend = ++starts == 1 ? initialBackend : ready;
    return haveBackend ? 0 : 1;
}
static void releasePictures(void) {
    for (unsigned n = 0; n < heldCount; ++n) {
        IHS_FrameOutcome o = {.result = IHS_VideoFrameResultDroppedReset};
        IHS_FrameTicketComplete(held[n], &o);
        IHS_FrameTicketRelease(held[n]);
    }
    heldCount = 0;
}
static void stop(IHS_Session *s, const IHS_VideoEpochInfo *e, void *x) {
    (void)s; (void)e; (void)x;
    if (haveBackend) simple("retire");
    haveBackend = false;
}
static void flush(IHS_Session *s, const IHS_VideoEpochInfo *e, IHS_StreamVideoFlushPhase phase,
                  void *x) {
    (void)s; (void)e; (void)x;
    if (phase == IHS_StreamVideoFlushPlayer) {
        simple("player");
        releasePictures();
    } else {
        assert(video && !video->frame.buffer.size && !video->frame.partial.head &&
               !video->frame.deferredCount && !video->assembly);
        if (recording && haveBackend) {
            FILE *f = event();
            fprintf(f, "[\"flush\",%u,%u,%u]", action, video->states.waitingKeyFrame,
                    video->states.syncingBacklog);
            assert(!fclose(f));
        }
    }
}
static IHS_StreamVideoSubmitResult submit(IHS_Session *s, const IHS_VideoEpochInfo *e, uint16_t id,
                                          IHS_FrameTicket *ticket, IHS_Buffer *b,
                                          IHS_StreamVideoFrameFlag flags, bool *taken, void *x) {
    (void)s; (void)e; (void)flags; (void)x;
#ifdef IHS_SUBFRAME_OBSERVER
    IHS_FrameIdentity identity = IHS_FrameTicketIdentity(ticket);
    assert(identity.frameId == id && identity.epoch == e->video_epoch);
#endif
    assert(heldCount < IHS_VIDEO_HELD_CAPACITY && IHS_FrameTicketRetain(ticket));
    held[heldCount] = ticket;
    pictures[heldCount++] = id;
    *taken = true;
    if (recording) {
        FILE *f = event();
        fprintf(f, "[\"decode\",%u,%u,\"", action, id);
        for (size_t n = 0; n < b->size; ++n) fprintf(f, "%02x", IHS_BufferPointer(b)[n]);
        fprintf(f, "\"]");
        assert(!fclose(f));
    }
    return IHS_StreamVideoSubmitOK;
}
static void feedRange(IHS_SessionChannel *ch, unsigned fid, unsigned seq, unsigned flags,
                      unsigned startRange, unsigned endRange, const uint8_t *bytes, size_t size) {
    IHS_Buffer b = IHS_BUFFER_INIT(64, 65536);
    IHS_BufferAppendUInt16LE(&b, seq);
    IHS_BufferAppendUInt8(&b, flags);
    IHS_BufferAppendUInt16LE(&b, startRange);
    IHS_BufferAppendUInt16LE(&b, endRange);
    IHS_BufferAppendMem(&b, bytes, size);
    IHS_SessionDataFrameHeader h = {.id = fid, .timestamp = 1000000};
    DataReceived(ch, &h, &b);
    /* Explicit consumer boundary, matching the Source action fixture. */
#ifdef IHS_SUBFRAME_OBSERVER
    while (DataThink(ch) == 1) {}
#else
    (void)DataThink(ch);
#endif
    IHS_BufferClear(&b, true);
}
static void feed(IHS_SessionChannel *ch, unsigned fid, unsigned seq, unsigned flags,
                 const uint8_t *bytes, size_t size) {
    feedRange(ch, fid, seq, flags, 0, 0, bytes, size);
}
int main(int argc, char **argv) {
    bool capacity = argc == 2 && !strcmp(argv[1], "--capacity");
    assert(capacity || argc == 5 || argc == 6);
    initialBackend = capacity ? true : atoi(argv[1]);
    int initial = capacity ? -1 : atoi(argv[2]);
    ready = capacity ? true : atoi(argv[3]);
    IHS_Init();
    IHS_Session *s = IHS_TestSessionCreate();
    assert(s);
    IHS_TimerTaskStopImmediate(s->retransmission.timer);
    s->state.connectionState = IHS_SessionConnectionStateConnected;
    assert(IHS_SessionSetVideoTrackingIdentity(s, 90));
    IHS_StreamVideoCallbacks cb = {.prepare = prepare, .startTracked = start,
                                   .submitTracked = submit, .stopTracked = stop, .flushTracked = flush};
    IHS_SessionSetVideoCallbacks(s, &cb, NULL);
    CStartVideoDataMsg c = CSTART_VIDEO_DATA_MSG__INIT;
    c.channel = 3; c.codec = argc == 6 ? atoi(argv[5]) : k_EStreamVideoCodecH264; c.width = 1280; c.height = 720;
    IHS_SessionChannel *ch = IHS_SessionChannelDataVideoCreate(s, &c);
    assert(ch);
    IHS_SessionChannelAdd(s, ch);
    video = (void *)ch;
    if (initial >= 0) {
        FILE *clip = fopen(argv[4], "rb");
        assert(clip);
        uint8_t packet[2204];
        assert(fread(packet, 1, sizeof(packet), clip) == sizeof(packet));
        assert(!fclose(clip));
        feed(ch, 0, 0, 24 | (initial ? 64 : 0), packet, sizeof(packet));
        assert(heldCount == 1);
    }
    if (capacity) {
        atomic_store(&video->resetRequested, true);
        for (unsigned n = 1; n <= IHS_FRAME_TRACKER_CAPACITY; ++n)
            feed(ch, 1, n - 1, 8, (const uint8_t *)"x", 1);
        assert(!s->base.interrupted && video->frame.deferredCount == IHS_FRAME_TRACKER_CAPACITY);
        feed(ch, 1, IHS_FRAME_TRACKER_CAPACITY, 8, (const uint8_t *)"x", 1);
        assert(s->base.interrupted);
        IHS_SessionDestroy(s);
        IHS_Quit();
        assert(!heldCount);
        return 0;
    }
    recording = true;
    char line[16384], raw[15000];
    while (fgets(line, sizeof(line), stdin)) {
        if (line[0] == 'F' || line[0] == 'S') {
            unsigned fid, seq, flags, startRange = 0, endRange = 0;
            if (line[0] == 'S') {
                assert(sscanf(line, "S %u %u %u %u %u %14999s", &fid, &seq, &flags,
                              &startRange, &endRange, raw) == 6);
            } else {
                assert(sscanf(line, "F %u %u %u %14999s", &fid, &seq, &flags, raw) == 4);
            }
            size_t len = !strcmp(raw, "-") ? 0 : strlen(raw) / 2;
            uint8_t bytes[7500];
            assert(!strcmp(raw, "-") || strlen(raw) == 2 * len);
            for (size_t n = 0; n < len; ++n) {
                unsigned v; assert(sscanf(raw + 2 * n, "%2x", &v) == 1); bytes[n] = v;
            }
            feedRange(ch, fid, seq, flags, startRange, endRange, bytes, len);
        } else if (line[0] == 'R') {
            IHS_SessionVideoHandlePendingReset(s);
        } else {
            assert(line[0] == 'Q');
            atomic_store(&video->resetRequested, true);
        }
        /* Local ownership invariant: deferred frames remain incomplete until
         * a key purge/actor closes them. This is not an Android pixel projection. */
        IHS_TrackedFrame stats[IHS_FRAME_TRACKER_CAPACITY];
        size_t count = IHS_FrameTrackerCopyStats(s->frameTracker, stats, IHS_FRAME_TRACKER_CAPACITY);
        for (size_t n = 0; n < video->frame.deferredCount; ++n) {
            IHS_FrameIdentity id = IHS_FrameTicketIdentity(video->frame.deferred[n]);
            bool found = false;
            for (size_t j = 0; j < count; ++j)
                if (stats[j].identity.receiveSerial == id.receiveSerial) {
                    assert(!stats[j].statsComplete);
                    found = true;
                }
            assert(found);
        }
        printf("{\"strict\":%u,\"soft\":%u,\"request\":%u,\"backend\":%s,\"mode\":[%u,%u],\"body\":\"",
               video->states.waitingKeyFrame, video->states.syncingBacklog,
               atomic_load(&video->resetRequested), haveBackend ? "true" : "false",
               video->states.contentModeSeen && !video->states.contentMode,
               video->states.contentModeSeen && video->states.contentMode);
        for (size_t n = 0; n < video->frame.buffer.size; ++n)
            printf("%02x", IHS_BufferPointer(&video->frame.buffer)[n]);
        printf("\",\"flow\":[");
        for (unsigned n = 0; n < flowCount; ++n) printf("%s%s", n ? "," : "", flow[n]);
        printf("],\"player\":[");
        for (unsigned n = 0; n < heldCount; ++n) printf("%s%u", n ? "," : "", pictures[n]);
#ifdef IHS_SUBFRAME_OBSERVER
        printf("],\"next_range\":%u,\"next_seq\":%u,\"overflow_span\":%d,\"partial\":[",
               video->frame.expectedSubFrameStart, video->states.expectedSequence,
               video->states.overflowFrameSpan);
        unsigned countParts = 0;
        for (IHS_VideoPartialFrame *part = video->frame.partial.head; part; part = part->next) {
            printf("%s[%u,%u,%u,%u,%u,\"", countParts++ ? "," : "", part->header.sequence,
                   part->frameId, part->header.flags, part->header.subFrameStart, part->header.subFrameEnd);
            for (size_t n = 0; n < part->data.size; ++n)
                printf("%02x", IHS_BufferPointer(&part->data)[n]);
            printf("\"]");
        }
#endif
        puts("]}");
        ++action;
    }
    recording = false;
    IHS_SessionDestroy(s);
    IHS_Quit();
    assert(!heldCount);
    for (unsigned n = 0; n < flowCount; ++n) free(flow[n]);
    return 0;
}
