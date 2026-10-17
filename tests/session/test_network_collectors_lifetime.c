/* Actual clock consumers plus local allocation/ownership protection. The
 * source fixture separately proves integer mean and collector arithmetic. */
#include "ihslib.h"
#include "session/clock.h"
#include <assert.h>
#include <stdlib.h>

static bool failNextAllocation;
void *__real_malloc(size_t);
void *__wrap_malloc(size_t size) {
    if (failNextAllocation) {
        failNextAllocation = false;
        return NULL;
    }
    return __real_malloc(size);
}
int main(void) {
    IHS_Init();
    IHS_StreamClock clock;
    IHS_StreamClockInit(&clock);
    IHS_StreamClockFeedback(&clock, 1000000 - 655, 1000000, 1000000);
    IHS_StreamClockFeedback(&clock, 1000010 - 656, 1000010, 1000010);
    assert(clock.ping.queue.count == 2 && clock.ping.sum == 1311);
    /* Original uint32 mean is 655 ticks. The old fractional mean (655.5)
     * incorrectly crossed both downstream integer-millisecond boundaries. */
    assert(IHS_StreamClockRTTMilliseconds(&clock) == 9.9945068359375f);
    assert(IHS_StreamClockNackAgeTicks(&clock) == 524);
    assert(IHS_StreamClockMediaDelayTicks(&clock) == 6880);
    IHS_StreamClockDeinit(&clock);

    IHS_NetworkPingStats ping = {0};
    IHS_NetworkBitrateStats rate = {0};
    IHS_NetworkPacketStats packets = {0};
    IHS_NetworkVideoStats video = {0};
    failNextAllocation = true;
    assert(!IHS_NetworkPingRecord(&ping, 655, 1000000));
    assert(!ping.queue.data && !ping.queue.count && !ping.sum);
    for (unsigned i = 0; i < 8; ++i) assert(IHS_NetworkBitrateRecord(&rate, 100 + i, 1));
    IHS_NetworkSample *data = rate.queue.data;
    failNextAllocation = true;
    assert(!IHS_NetworkBitrateRecord(&rate, 400000, 3));
    assert(rate.queue.data == data && rate.queue.count == 8 && rate.bytes == 8);
    assert(IHS_NetworkBitrateRecord(&rate, 400000, 3));
    assert(rate.queue.count == 1 && rate.bytes == 3);
    failNextAllocation = true;
    assert(!IHS_NetworkPacketRecord(&packets, 100, 2, true));
    assert(!packets.total && !packets.lost && !packets.queue.data);
    assert(IHS_NetworkVideoRecord(&video, 100, 100, 110, 1, 30));
    failNextAllocation = true;
    assert(!IHS_NetworkVideoRecord(&video, 200, 200, 210, 2, 40));
    assert(video.frame == 1 && video.firstSent == 100 && video.lastReceived == 110);
    assert(IHS_NetworkVideoRecord(&video, 200, 200, 210, 2, 40));
    assert(video.queue.count == 1 && video.frame == 2);
    IHS_NetworkSampleQueueDestroy(&ping.queue);
    IHS_NetworkSampleQueueDestroy(&rate.queue);
    IHS_NetworkSampleQueueDestroy(&packets.queue);
    IHS_NetworkSampleQueueDestroy(&video.queue);
    IHS_Quit();
    return 0;
}
