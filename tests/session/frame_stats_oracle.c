/* Controlled entry points for the official-client ring/Save vectors. This is
 * not the video worker, complete stats packet, accumulator or transport oracle. */
#include "session/frame_stats.h"
#include "session/session_pri.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t now = 1000000;
uint32_t __wrap_IHS_SessionPacketTimestamp(void) {
    return now;
}
static void hex(const uint8_t *p, size_t size) {
    for (size_t i = 0; i < size; ++i)
        printf("%02x", p[i]);
}

int main(int argc, char **argv) {
    assert(argc >= 5);
    IHS_FrameStatsAggregator *agg = IHS_FrameStatsAggregatorCreate();
    assert(agg);
    IHS_Session session = {.frameStats = agg};
    agg->fullReporting = atoi(argv[1]) != 0;
    int32_t offset = (int32_t)strtol(argv[2], NULL, 10);
    agg->lastSentFrameId = agg->lastDisplayedFrameId = (uint16_t)atoi(argv[3]);
    IHS_FrameStatsSlot *drained = calloc(IHS_FRAME_STATS_RING_SIZE, sizeof(*drained));
    assert(drained);
    char line[256];
    while (fgets(line, sizeof(line), stdin)) {
        char op;
        unsigned id, a, b, c, d, e, f;
        assert(sscanf(line, " %c", &op) == 1);
        size_t count = 0;
        if (op == 'R') {
            assert(sscanf(line + 1, "%u %u %u %u %u %u %u", &id, &a, &b, &c, &d, &e, &f) == 7);
            IHS_FrameStatsRecordReceivedInput(agg, id, a, b, c, d, e, f);
        } else if (op == 'E') {
            assert(sscanf(line + 1, "%u %u %u", &id, &a, &b) == 3);
            IHS_SessionReportVideoFrameStage(&session, id, (IHS_VideoFrameStage)a, b);
        } else if (op == 'C') {
            assert(sscanf(line + 1, "%u %u %u", &id, &a, &now) == 3);
            IHS_SessionReportVideoFrameComplete(&session, id, (IHS_VideoFrameResult)a);
        } else if (op == 'I') {
            /* Legacy vector's explicit input table seed. M executes the real
             * native CreateInputMark and is compared with original code. */
            assert(sscanf(line + 1, "%u %u %u", &id, &a, &b) == 3);
            agg->lastInputMark = id;
            agg->inputMarks[id & 1023u].id = id;
            agg->inputMarks[id & 1023u].begin = a;
            agg->inputMarks[id & 1023u].sent = b;
        } else if (op == 'M') {
            assert(sscanf(line + 1, "%u %u %u", &id, &a, &now) == 3);
            agg->lastInputMark = id;
            assert(IHS_FrameStatsCreateInputMark(agg, a) == (id == 65535 ? 1 : id + 1));
        } else {
            assert(op == 'S');
            count =
                IHS_FrameStatsAggregatorDrainSlots(agg, drained, IHS_FRAME_STATS_RING_SIZE, NULL);
            /* Accumulator comparisons are explicitly outside this harness. */
            IHS_FrameStatsAccumulatorReset(&agg->accumulator);
        }
        printf(
            "{\"last_frame\":%u,\"last_display\":%u,\"displayed\":%u,\"reported\":%u,\"slots\":[",
            agg->previousFrameStart, agg->previousDisplay, agg->lastDisplayedFrameId,
            agg->lastSentFrameId);
        for (int i = 4; i < argc; ++i) {
            uint16_t fid = (uint16_t)atoi(argv[i]);
            IHS_FrameStatsSlot *s = &agg->ring[fid % IHS_FRAME_STATS_RING_SIZE];
            if (i != 4)
                putchar(',');
            if (s->frameId != fid) {
                printf("null");
                continue;
            }
            printf("{\"id\":%u,\"size\":%u,\"input\":%u,\"events\":[", s->frameId, s->frameSize,
                   s->inputMark);
            for (unsigned j = 0; j < IHS_FRAME_STATS_EVENT_COUNT; ++j)
                printf("%s%u", j ? "," : "", s->events[j]);
            printf("],\"result\":%d,\"deltas\":\"", s->result);
            hex((const uint8_t *)&s->frameStartDelta, sizeof(float));
            hex((const uint8_t *)&s->frameDisplayDelta, sizeof(float));
            printf("\"}");
        }
        printf("],\"details\":[");
        if (agg->fullReporting) {
            for (size_t i = 0; i < count; ++i) {
                CFrameStats row;
                CFrameEvent events[IHS_FRAME_STATS_EVENT_COUNT];
                CFrameEvent *pointers[IHS_FRAME_STATS_EVENT_COUNT];
                IHS_FrameStatsEncodeFrame(&drained[i], offset, &row, events, pointers);
                uint8_t bytes[512];
                assert(cframe_stats__get_packed_size(&row) <= sizeof(bytes));
                size_t size = cframe_stats__pack(&row, bytes);
                printf("%s\"", i ? "," : "");
                hex(bytes, size);
                putchar('"');
            }
        }
        puts("]}");
    }
    free(drained);
    IHS_FrameStatsAggregatorDestroy(agg);
    return 0;
}
