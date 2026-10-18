/* Actual production video TU, assembly/tickets and SDK-phase callbacks. Initial
 * pending/strict/soft are decoder-boundary inputs. OS worker stays idle while
 * deterministic consumer actions run; physical SDK output is not modelled. */
#include "../../src/session/channels/video/ch_data_video.c"
#include "ihs_buffer_ext.h"
#include "test_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static bool backend, recording;
static unsigned action, flowCount, heldCount;
static char *flow[64];
static IHS_FrameTicket *held[32];
static IHS_SessionChannelVideo *video;
uint32_t __wrap_IHS_SessionPacketTimestamp(void) {
    return 1000000;
}
bool __wrap_IHS_SessionSendControlMessage(IHS_Session *s, EStreamControlMessage t,
                                          const ProtobufCMessage *m) {
    (void)s;
    (void)t;
    (void)m;
    return true;
}
void __wrap_IHS_SessionChannelDataLost(IHS_SessionChannel *c) {
    (void)c;
}
static int prepare(IHS_Session *s, const IHS_StreamVideoConfig *c, void *x) {
    (void)s;
    (void)c;
    (void)x;
    return 0;
}
static int start(IHS_Session *s, const IHS_VideoEpochInfo *e, const IHS_StreamVideoConfig *c,
                 void *x) {
    (void)s;
    (void)e;
    (void)c;
    (void)x;
    return backend ? 0 : 1;
}
static void stop(IHS_Session *s, const IHS_VideoEpochInfo *e, void *x) {
    (void)s;
    (void)e;
    (void)x;
    for (unsigned n = 0; n < heldCount; ++n) {
        IHS_FrameOutcome o = {.result = IHS_VideoFrameResultDisplayed};
        IHS_FrameTicketComplete(held[n], &o);
        IHS_FrameTicketRelease(held[n]);
    }
    heldCount = 0;
}
static FILE *event(void) {
    assert(flowCount < 64);
    size_t size;
    FILE *f = open_memstream(&flow[flowCount++], &size);
    assert(f);
    return f;
}
static void flush(IHS_Session *s, const IHS_VideoEpochInfo *e, IHS_StreamVideoFlushPhase phase,
                  void *x) {
    (void)s;
    (void)e;
    (void)x;
    if (recording && phase == IHS_StreamVideoFlushDecoder) {
        assert(video && !video->frame.buffer.size && !video->frame.partial.head);
        if (backend) {
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
    (void)s;
    (void)e;
    (void)flags;
    (void)x;
    assert(heldCount < 32 && IHS_FrameTicketRetain(ticket));
    held[heldCount++] = ticket;
    *taken = true;
    if (!recording)
        return IHS_StreamVideoSubmitOK;
    FILE *f = event();
    fprintf(f, "[\"decode\",%u,%u,\"", action, id);
    for (size_t n = 0; n < b->size; ++n)
        fprintf(f, "%02x", IHS_BufferPointer(b)[n]);
    fprintf(f, "\"]");
    assert(!fclose(f));
    return IHS_StreamVideoSubmitOK;
}
static void feed(IHS_SessionChannel *ch, unsigned fid, unsigned seq, unsigned flags,
                 const uint8_t *bytes, size_t size) {
    IHS_Buffer b = IHS_BUFFER_INIT(64, 65536);
    IHS_BufferAppendUInt16LE(&b, seq);
    IHS_BufferAppendUInt8(&b, flags);
    IHS_BufferAppendUInt16LE(&b, 0);
    IHS_BufferAppendUInt16LE(&b, 0);
    IHS_BufferAppendMem(&b, bytes, size);
    IHS_SessionDataFrameHeader h = {.id = fid, .timestamp = 1000000};
    DataReceived(ch, &h, &b);
    IHS_BufferClear(&b, true);
}
int main(int argc, char **argv) {
    assert(argc == 6);
    backend = atoi(argv[1]);
    IHS_Init();
    IHS_Session *s = IHS_TestSessionCreate();
    assert(s);
    IHS_TimerTaskStopImmediate(s->retransmission.timer);
    s->state.connectionState = IHS_SessionConnectionStateConnected;
    assert(IHS_SessionSetVideoTrackingIdentity(s, 90));
    IHS_StreamVideoCallbacks cb = {.prepare = prepare,
                                   .startTracked = start,
                                   .submitTracked = submit,
                                   .stopTracked = stop,
                                   .flushTracked = flush};
    IHS_SessionSetVideoCallbacks(s, &cb, NULL);
    CStartVideoDataMsg c = CSTART_VIDEO_DATA_MSG__INIT;
    c.channel = 3;
    c.codec = k_EStreamVideoCodecH264;
    c.width = 1280;
    c.height = 720;
    IHS_SessionChannel *ch = IHS_SessionChannelDataVideoCreate(s, &c);
    assert(ch);
    IHS_SessionChannelAdd(s, ch);
    video = (void *)ch;
    if (backend) {
        FILE *clip = fopen(argv[5], "rb");
        assert(clip);
        uint8_t packet[2204];
        assert(fread(packet, 1, sizeof(packet), clip) == sizeof(packet));
        assert(!fclose(clip));
        feed(ch, 0, 0, 24, packet, sizeof(packet));
        assert(heldCount == 1);
    }
    if (strcmp(argv[4], "none"))
        feed(ch, 1, backend ? 1 : 0, !strcmp(argv[4], "encoded") ? 2 : 0, (const uint8_t *)"old-partial", 11);
    video->states.waitingKeyFrame = atoi(argv[2]);
    video->states.syncingBacklog = atoi(argv[3]);
    recording = true;
    char line[16384], hex[15000];
    unsigned fid, seq, flags;
    while (fgets(line, sizeof(line), stdin)) {
        assert(sscanf(line, "%u %u %u %14999s", &fid, &seq, &flags, hex) == 4);
        size_t len = strlen(hex) / 2;
        uint8_t bytes[7500];
        assert(strlen(hex) == len * 2);
        for (size_t n = 0; n < len; ++n) {
            unsigned b;
            assert(sscanf(hex + n * 2, "%2x", &b) == 1);
            bytes[n] = b;
        }
        feed(ch, fid, seq, flags, bytes, len);
        printf("{\"strict\":%u,\"soft\":%u,\"request\":%u,\"flow\":[",
               video->states.waitingKeyFrame, video->states.syncingBacklog,
               atomic_load(&video->resetRequested));
        for (unsigned n = 0; n < flowCount; ++n)
            printf("%s%s", n ? "," : "", flow[n]);
        puts("]}");
        ++action;
    }
    recording = false;
    IHS_SessionDestroy(s);
    IHS_Quit();
    for (unsigned n = 0; n < flowCount; ++n)
        free(flow[n]);
    return 0;
}
