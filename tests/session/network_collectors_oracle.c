/* Native collector/window states compared with pinned official instructions.
 * No Socket, session admission or decoder caller is simulated here. */
#include "session/network_stats.h"
#include "session/clock.h"
#include "ihslib.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned bits(float value) {
    unsigned result;
    memcpy(&result, &value, sizeof(result));
    return result;
}
static void word(uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) printf("%02x", (value >> (8 * i)) & 255);
}
static void samples(const IHS_NetworkSampleQueue *q, unsigned values) {
    printf("[");
    for (size_t i = 0; i < q->count; ++i) {
        const IHS_NetworkSample *sample = &q->data[(q->head + i) & (q->capacity - 1)];
        printf("%s\"", i ? "," : "");
        word(sample->timestamp);
        for (unsigned j = 0; j < values; ++j) word(sample->values[j]);
        printf("\"");
    }
    printf("]");
}
int main(void) {
    IHS_NetworkBitrateStats rates[2] = {0};
    IHS_NetworkPacketStats packets = {0};
    IHS_Init();
    IHS_StreamClock clock;
    IHS_StreamClockInit(&clock);
    IHS_NetworkPingStats *ping = &clock.ping;
    IHS_NetworkVideoStats video = {0};
    char line[256];
    while (fgets(line, sizeof(line), stdin)) {
        unsigned tick, direction, lost, value, b, c, frame;
        int count;
        switch (line[0]) {
        case 'B':
            assert(sscanf(line, "B %u %u %d", &direction, &tick, &count) == 3 && direction < 2);
            assert(IHS_NetworkBitrateRecord(&rates[direction], tick, count));
            break;
        case 'P':
            assert(sscanf(line, "P %u %d %u", &tick, &count, &lost) == 3);
            assert(IHS_NetworkPacketRecord(&packets, tick, count, lost));
            break;
        case 'G':
            assert(sscanf(line, "G %u %u", &value, &tick) == 2);
            assert(IHS_NetworkPingRecord(ping, value, tick));
            break;
        case 'T':
            assert(sscanf(line + (line[1] == 'C' ? 2 : 1), " %u %u %u", &value, &b, &tick) == 3);
            IHS_StreamClockFeedback(&clock, value, b, tick);
            break;
        case 'V':
            assert(sscanf(line, "V %u %u %u %u %d", &tick, &b, &c, &frame, &count) == 5);
            assert(IHS_NetworkVideoRecord(&video, tick, b, c, frame, count));
            break;
        case 'F': case 'W':
            /* Source also executes FrameReceived's inline collector. Native
             * supplies those normalized fields to this component, not RX. */
            assert(sscanf(line + 1, " %u %u %u %d", &frame, &tick, &c, &count) == 4);
            assert(IHS_NetworkVideoRecord(&video, tick, line[0] == 'W' ? c : tick, c, frame, count));
            break;
        default: assert(false);
        }
        printf("{\"rates\":[");
        for (unsigned i = 0; i < 2; ++i) {
            printf("%s{\"sum\":%llu,\"bits\":%u,\"samples\":", i ? "," : "",
                   (unsigned long long)rates[i].bytes, bits(rates[i].bitrate));
            samples(&rates[i].queue, 1);
            printf("}");
        }
        printf("],\"packets\":{\"total\":%u,\"lost\":%u,\"samples\":", packets.total, packets.lost);
        samples(&packets.queue, 2);
        printf("},\"ping\":{\"sum\":%u,\"bits\":%u,\"samples\":", ping->sum, bits(ping->milliseconds));
        samples(&ping->queue, 1);
        printf("},\"video\":{\"frame\":%u,\"pending\":[%u,%u,%u],\"totals\":[%u,%u,%u,%u],"
               "\"bits\":[%u,%u],\"samples\":", video.frame, video.firstSent, video.lastReceived,
               video.frameBytes, video.totals[0], video.totals[1], video.totals[2], video.totals[3],
               bits(video.bitrate), bits(video.link));
        samples(&video.queue, 4);
        printf("}");
        if (line[0] == 'T') {
            printf(",\"clock\":{\"echo\":%u,\"minimum\":%u,\"smooth\":%u,"
                   "\"next_min_update\":%u,\"offset\":%u", clock.lastEcho, clock.minRTT,
                   clock.smoothedRTT, clock.minRTTUpdate, (uint32_t)clock.offset);
            if (line[1] == 'C') printf(",\"offset_count\":%u,\"offset_sum\":%llu", clock.samples,
                                      (unsigned long long)(uint64_t)clock.offsetSum);
            printf("}");
        }
        printf("}\n");
    }
    for (unsigned i = 0; i < 2; ++i) IHS_NetworkSampleQueueDestroy(&rates[i].queue);
    IHS_NetworkSampleQueueDestroy(&packets.queue);
    IHS_StreamClockDeinit(&clock);
    IHS_NetworkSampleQueueDestroy(&video.queue);
    IHS_Quit();
    return 0;
}
