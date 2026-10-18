/* Real session stage -> frame-stat consumer -> protobuf/StatsSend/control
 * producer -> reliable frame admission. Clock and final admission rejection
 * are controlled. Whole socket/decoder scheduling is outside this oracle. */
#include "session/channels/ch_control.h"
#include "session/frame_stats.h"
#include "test_session.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static IHS_Session *session;
static uint32_t now;
static bool rejectStats;
static unsigned attempts, count;
static uint64_t sequence;
static struct { unsigned channel; char *body; } frames[16];

uint32_t __wrap_IHS_SessionPacketTimestamp(void) { return now; }
bool __real_IHS_SessionSendFrameEvents(IHS_Session *, IHS_FrameStatsMessage *);
bool __wrap_IHS_SessionSendFrameEvents(IHS_Session *s, IHS_FrameStatsMessage *scratch) {
    ++attempts;
    return __real_IHS_SessionSendFrameEvents(s, scratch);
}
IHS_SendAdmission __real_IHS_SessionChannelQueueFrameResult(IHS_SessionChannel *,
                                                            IHS_SessionFrame *, bool);
IHS_SendAdmission __wrap_IHS_SessionChannelQueueFrameResult(IHS_SessionChannel *channel,
                                                            IHS_SessionFrame *frame,
                                                            bool reliable) {
    assert(reliable && count < 16);
    IHS_Buffer body = frame->body, plain = IHS_BUFFER_INIT(1024, 0);
    uint8_t tag = *IHS_BufferPointer(&body);
    IHS_BufferOffsetBy(&body, 1);
    if (channel->id == 1) {
        uint64_t actualSequence;
        assert(!IHS_SessionFrameDecrypt(session, &body, &plain, sequence++, &actualSequence));
        body = plain;
    } else {
        assert(channel->id == 2 && tag == 1);
    }
    frames[count].channel = channel->id;
    frames[count].body = malloc(body.size * 2 + 3);
    assert(frames[count].body);
    sprintf(frames[count].body, "%02x", tag);
    for (size_t i = 0; i < body.size; ++i)
        sprintf(frames[count].body + 2 + i * 2, "%02x", IHS_BufferPointer(&body)[i]);
    ++count;
    IHS_BufferClear(&plain, true);
    if (channel->id == 2 && rejectStats)
        return IHS_SendAdmissionNotConnected;
    return __real_IHS_SessionChannelQueueFrameResult(channel, frame, reliable);
}
static void report(void) {
    IHS_SessionChannelControl *control = (void *)session->channels[1];
    printf("{\"state\":%d,\"frames\":[", session->state.connectionState);
    for (unsigned i = 0; i < count; ++i) {
        printf("%s{\"channel\":%u,\"body\":\"%s\"}", i ? "," : "",
               frames[i].channel, frames[i].body);
        free(frames[i].body);
    }
    printf("],\"frame_events\":[");
    for (unsigned i = 0; i < attempts; ++i)
        printf("%s1", i ? "," : "");
    printf("],\"frame_events_time\":%u,\"keepalive_time\":%u,\"sequence\":%llu,"
           "\"reported\":%u}\n", control->frameEventsTimestamp, control->keepAliveTimestamp,
           (unsigned long long)control->sendEncryptSequence, session->frameStats->lastSentFrameId);
    count = attempts = 0;
}
int main(int argc, char **argv) {
    assert(argc == 4);
    IHS_Init();
    session = IHS_TestSessionCreate();
    assert(session && session->frameStats);
    IHS_TimerTaskStopImmediate(session->retransmission.timer);
    session->state.connectionState = atoi(argv[1]);
    session->state.connectionId = 12;
    session->state.hostConnectionId = 34;
    rejectStats = atoi(argv[3]) != 0;
    IHS_FrameStatsAggregatorSetFullReporting(session->frameStats, atoi(argv[2]) != 0);
    char line[128];
    while (fgets(line, sizeof(line), stdin)) {
        unsigned id, result;
        if (sscanf(line, "R %u", &id) == 1)
            IHS_FrameStatsRecordReceived(session->frameStats, id, 900000, 901000, 902000, 100, 0);
        else if (sscanf(line, "C %u %u %u", &id, &result, &now) == 3)
            IHS_FrameStatsRecordCompleteAt(session->frameStats, id, result, now);
        else {
            assert(sscanf(line, "UPDATE %u", &now) == 1);
            IHS_SessionChannelControlUpdateStage(session->channels[1], now);
            report();
        }
    }
    IHS_SessionDestroy(session);
    IHS_Quit();
    return 0;
}
