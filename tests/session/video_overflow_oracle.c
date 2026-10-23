/* Original-client boundary vectors enter the actual production overflow
 * consumer. Valid two-endpoint lists and client phases are explicit inputs;
 * no worker, socket, decoder hardware or full session lifetime is simulated. */
#include "../../src/session/channels/video/ch_data_video.c"
#include <inttypes.h>
#include <stdio.h>

static IHS_Session fixture;
static unsigned feedbackCount;
static uint32_t phaseAfterFeedback;

uint32_t __wrap_IHS_SessionPacketTimestamp(void) { return 1000000; }
static void feedback(unsigned channel, const uint8_t *bytes, size_t size) {
    printf("%s[%u,\"", feedbackCount++ ? "," : "", channel);
    for (size_t n = 0; n < size; ++n) printf("%02x", bytes[n]);
    printf("\"]");
    /* Explicit transition at delivery boundary, not a real thread race. */
    if (phaseAfterFeedback != UINT32_MAX)
        fixture.state.connectionState = phaseAfterFeedback;
}
bool __wrap_IHS_SessionSendControlMessage(IHS_Session *session, EStreamControlMessage type,
                                          const ProtobufCMessage *message) {
    assert(session == &fixture && type == k_EStreamControlVideoOverflow);
    uint8_t bytes[32] = {k_EStreamControlVideoOverflow};
    size_t size = protobuf_c_message_get_packed_size(message);
    assert(size < sizeof(bytes) && protobuf_c_message_pack(message, bytes + 1) == size);
    feedback(1, bytes, size + 1);
    return true;
}
bool __wrap_IHS_SessionChannelQueuePacket(IHS_SessionChannel *channel, IHS_SessionPacket *packet,
                                          bool retransmit) {
    assert(channel->session == &fixture && !retransmit && packet->header.channelId == 3);
    feedback(3, IHS_BufferPointer(&packet->body), packet->body.size);
    return true;
}
int main(void) {
    IHS_SessionChannelVideo video = {0};
    IHS_SessionChannel *channel = &video.base.base;
    channel->session = &fixture;
    channel->id = 3;
    IHS_VideoPartialFrame head = {0}, tail = {0};
    char line[256];
    unsigned phase, waiting, syncing, count, first, last, ticks, previous, hasPrevious;
    while (fgets(line, sizeof(line), stdin)) {
        assert(sscanf(line, "%u %u %u %u %u %u %u %u %u %" SCNu32,
                      &phase, &waiting, &syncing, &count, &first, &last, &ticks, &previous,
                      &hasPrevious, &phaseAfterFeedback) == 10);
        assert(phase <= 7 && count <= 2 && first <= UINT16_MAX && last <= UINT16_MAX);
        fixture.state.connectionState = phase;
        video.states.waitingKeyFrame = waiting;
        video.states.syncingBacklog = syncing;
        if (hasPrevious) video.states.overflowFrameSpan = (int32_t)previous;
        head.frameId = first; tail.frameId = last;
        head.timestamp = 1000000; tail.timestamp = 1000000 + ticks;
        head.next = count == 2 ? &tail : NULL;
        tail.prev = &head;
        video.frame.partial.head = count ? &head : NULL;
        video.frame.partial.tail = count == 2 ? &tail : count == 1 ? &head : NULL;
        feedbackCount = 0;
        printf("{\"feedback\":[");
        CheckPartialOverflow(channel);
        printf("],\"span\":%u,\"syncing\":%u,\"last_recovery\":%u,\"phase\":%d}\n",
               (uint32_t)video.states.overflowFrameSpan, video.states.syncingBacklog,
               video.states.lastRecoveryRequest, fixture.state.connectionState);
    }
    return 0;
}
