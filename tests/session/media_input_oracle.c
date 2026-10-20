/* Real complete-frame dispatch/RX preparation -> per-media stats/shared marks
 * -> actual video report/protobuf/admission. Decoder enqueue, mark counter,
 * clocks and network-sample boundary are controlled; no device/worker claim. */
#include "ihs_buffer.h"
#include "ihs_buffer_ext.h"
#include "ihslib.h"
#include "session/channels/ch_data.h"
#include "session/frame_stats.h"
#include "session/reliable.h"
#include "session/session_pri.h"
#include "test_session.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t now = 1000000;
static char *frameBody;
static unsigned videoSamples, admitted;
uint32_t __wrap_IHS_SessionPacketTimestamp(void) {
    return now;
}
void __real_IHS_SessionRecordNetworkVideo(IHS_Session *, uint32_t, uint32_t, uint32_t, uint16_t,
                                          uint32_t);
void __wrap_IHS_SessionRecordNetworkVideo(IHS_Session *s, uint32_t sent, uint32_t begin,
                                          uint32_t end, uint16_t id, uint32_t size) {
    ++videoSamples;
    __real_IHS_SessionRecordNetworkVideo(s, sent, begin, end, id, size);
}
void __wrap_IHS_SessionChannelDataReceiveReliableFrame(IHS_SessionChannel *ch,
                                                       IHS_SessionFrame *f) {
    assert(ch->id == 3 || ch->id == 4);
    ++admitted;
    assert(f->mediaPrepared); /* Observe the actual pre-FIFO preparation. */
}
IHS_SendAdmission __real_IHS_SessionChannelQueueFrameResult(IHS_SessionChannel *,
                                                            IHS_SessionFrame *, bool);
IHS_SendAdmission __wrap_IHS_SessionChannelQueueFrameResult(IHS_SessionChannel *ch,
                                                            IHS_SessionFrame *f, bool reliable) {
    assert(ch->id == 2 && reliable && !frameBody);
    frameBody = malloc(f->body.size * 2 + 1);
    assert(frameBody);
    for (size_t i = 0; i < f->body.size; ++i)
        sprintf(frameBody + i * 2, "%02x", IHS_BufferPointer(&f->body)[i]);
    frameBody[f->body.size * 2] = 0;
    return __real_IHS_SessionChannelQueueFrameResult(ch, f, reliable);
}
static void slots(const IHS_FrameStatsSlot *ring, int argc, char **argv) {
    putchar('[');
    for (int i = 3; i < argc; ++i) {
        uint16_t id = (uint16_t)atoi(argv[i]);
        const IHS_FrameStatsSlot *s = &ring[id % IHS_FRAME_STATS_RING_SIZE];
        if (i > 3)
            putchar(',');
        if (s->frameId != id) {
            printf("null");
            continue;
        }
        printf("{\"id\":%u,\"size\":%u,\"input\":%u,\"result\":%u,\"events\":[", id, s->frameSize,
               s->inputMark, (unsigned)s->result);
        for (unsigned e = 0; e < IHS_FRAME_STATS_EVENT_COUNT; ++e)
            printf("%s%u", e ? "," : "", s->events[e]);
        printf("],\"deltas\":\"");
        const uint8_t *delta = (const void *)&s->frameStartDelta;
        for (unsigned d = 0; d < 8; ++d)
            printf("%02x", delta[d]);
        printf("\"}");
    }
    putchar(']');
}
int main(int argc, char **argv) {
    assert(argc >= 4);
    IHS_Init();
    IHS_Session *s = IHS_TestSessionCreate();
    assert(s);
    IHS_TimerTaskStopImmediate(s->retransmission.timer);
    s->state.connectionState = IHS_SessionConnectionStateConnected;
    s->state.connectionId = 12;
    s->state.hostConnectionId = 34;
    s->clock.offset = (int32_t)strtol(argv[2], NULL, 10);
    IHS_FrameStatsAggregator *agg = s->frameStats;
    IHS_FrameStatsAggregatorSetFullReporting(agg, atoi(argv[1]) != 0);
    /* Real endpoint lookup/admission; no decoder exists at the Source boundary. */
    const IHS_SessionChannelClass cls = {.instanceSize = sizeof(IHS_SessionChannel)};
    for (unsigned type = 0; type < 2; ++type) {
        IHS_SessionChannel *ch = IHS_SessionChannelCreate(
            &cls, s, type ? IHS_SessionChannelTypeDataVideo : IHS_SessionChannelTypeDataAudio,
            3 + type, NULL);
        assert(ch);
        IHS_SessionChannelAdd(s, ch);
    }
    bool watched[1024] = {false};
    IHS_FrameStatsMessage *message = calloc(1, sizeof(*message));
    assert(message);
    char line[256];
    while (fgets(line, sizeof(line), stdin)) {
        char op;
        unsigned typ, id, start, sent, received, size, mark, input;
        assert(sscanf(line, " %c", &op) == 1);
        if (op == 'M') {
            assert(sscanf(line + 1, "%u %u %u", &id, &start, &now) == 3);
            agg->lastInputMark = (uint16_t)id;
            uint16_t value = IHS_FrameStatsCreateInputMark(agg, start);
            assert(value == (id == 65535 ? 1 : id + 1));
            watched[value & 1023] = true;
        } else if (op == 'R') {
            assert(sscanf(line + 1, "%u %u %u %u %u %u %u %u", &typ, &id, &start, &sent, &received,
                          &size, &mark, &input) == 8);
            assert(typ < 2 && size < 1024);
            IHS_SessionFrame f = {.header = {.type = IHS_SessionPacketTypeReliable,
                                             .channelId = 3 + typ,
                                             .sendTimestamp = sent,
                                             .receiveTimestamp = received,
                                             .receiveBegin = received}};
            IHS_BufferInit(&f.body, 1024, 4096);
            IHS_BufferAppendUInt8(&f.body, k_EStreamDataPacket);
            IHS_BufferAppendUInt16LE(&f.body, (uint16_t)id);
            IHS_BufferAppendUInt32LE(&f.body, start);
            IHS_BufferAppendUInt16LE(&f.body, (uint16_t)mark);
            IHS_BufferAppendUInt32LE(&f.body, input);
            for (unsigned n = 0; n < size; ++n)
                IHS_BufferAppendUInt8(&f.body, 0);
            IHS_ReliableDisposition disposition = IHS_SessionDeliverReliableFrame(s, &f);
            assert(disposition == IHS_ReliableData || disposition == IHS_ReliableDiscarded);
            IHS_BufferClear(&f.body, true);
        } else if (op == 'Q') {
            assert(sscanf(line + 1, "%u %u", &typ, &id) == 2 && typ <= 7 && id <= 1);
            s->state.connectionState = (IHS_SessionConnectionState)typ;
            s->transport.clientPaused = id != 0;
        } else if (op == 'C') {
            assert(sscanf(line + 1, "%u %u %u", &id, &typ, &now) == 3);
            IHS_SessionReportVideoFrameComplete(s, (uint16_t)id, (IHS_VideoFrameResult)typ);
        } else {
            assert(op == 'S');
            IHS_SessionSendFrameEvents(s, message);
        }
        printf("{\"audio_slots\":");
        slots(agg->audioRing, argc, argv);
        printf(",\"video\":{\"last_frame\":%u,\"last_display\":%u,\"displayed\":%u,\"reported\":%u,"
               "\"slots\":",
               agg->previousFrameStart, agg->previousDisplay, agg->lastDisplayedFrameId,
               agg->lastSentFrameId);
        slots(agg->ring, argc, argv);
        printf("},\"marks\":[");
        bool comma = false;
        for (unsigned n = 0; n < 1024; ++n)
            if (watched[n]) {
                printf("%s[%u,%u,%u,%u]", comma ? "," : "", n, agg->inputMarks[n].id,
                       agg->inputMarks[n].begin, agg->inputMarks[n].sent);
                comma = true;
            }
        printf("],\"frames\":[%s%s%s],\"video_samples\":%u,\"admitted\":%u}\n",
               frameBody ? "\"" : "", frameBody ? frameBody : "", frameBody ? "\"" : "",
               videoSamples, admitted);
        videoSamples = admitted = 0;
        free(frameBody);
        frameBody = NULL;
    }
    free(message);
    IHS_SessionDestroy(s);
    IHS_Quit();
    return 0;
}
