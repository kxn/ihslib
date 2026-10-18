/* Real public event callbacks -> report -> protobuf/StatsSend -> frame admission.
 * Clock, network collector snapshot and final frame admission are controlled.
 * Tracked modes additionally use real ticket stage/publication and collection.
 * This is not a decoder/OnThink, metric-collector-window or Socket oracle. */
#include "ihslib.h"
#include "session/frame_stats.h"
#include "session/frame_tracker.h"
#include "session/session_pri.h"
#include "test_session.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t now = 1000000;
static char *frameBody;
static int sendResult;
static uint64_t capture_us(uint32_t tick) {
    return ((uint64_t)tick * 1000000u + 65535u) / 65536u;
}
uint32_t __wrap_IHS_SessionPacketTimestamp(void) {
    return now;
}
static void hex(const uint8_t *p, size_t size) {
    for (size_t i = 0; i < size; ++i)
        printf("%02x", p[i]);
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
    return sendResult ? IHS_SendAdmissionNotConnected
                      : __real_IHS_SessionChannelQueueFrameResult(ch, f, reliable);
}

int main(int argc, char **argv) {
    assert(argc >= 5);
    bool tracked = !strncmp(argv[1], "tracked", 7);
    bool batched = !strcmp(argv[1], "tracked-batched");
    if (tracked) {
        ++argv;
        --argc;
    }
    IHS_Init();
    IHS_Session *session = IHS_TestSessionCreate();
    assert(session);
    IHS_TimerTaskStopImmediate(session->retransmission.timer);
    session->state.connectionState = IHS_SessionConnectionStateConnected;
    session->state.connectionId = 12;
    session->state.hostConnectionId = 34;
    IHS_FrameStatsAggregator *agg = session->frameStats;
    IHS_FrameTicket **tickets = NULL;
    uint64_t presented = 0;
    if (tracked) {
        assert(IHS_SessionSetVideoTrackingIdentity(session, 1));
        assert(IHS_FrameTrackerOpenEpoch(session->frameTracker, 1));
        tickets = calloc(65536, sizeof(*tickets));
        assert(tickets);
    }
    agg->fullReporting = atoi(argv[1]) != 0;
    int32_t offset = (int32_t)strtol(argv[2], NULL, 10);
    session->clock.offset = offset;
    agg->lastSentFrameId = agg->lastDisplayedFrameId = (uint16_t)atoi(argv[3]);
    IHS_FrameStatsMessage *message = calloc(1, sizeof(*message));
    assert(message);
    char line[256];
    while (fgets(line, sizeof(line), stdin)) {
        char op;
        unsigned id, a, b, c, d, e, f;
        assert(sscanf(line, " %c", &op) == 1);
        if (op == 'R') {
            assert(sscanf(line + 1, "%u %u %u %u %u %u %u", &id, &a, &b, &c, &d, &e, &f) == 7);
            if (tracked)
                IHS_FrameStatsCollectTracked(agg, session->frameTracker);
            IHS_FrameStatsRecordReceivedInput(agg, id, a, b, c, d, e, f);
            if (tracked) {
                IHS_FrameReceive receive = {.firstReceiveUs = capture_us(c),
                                            .lastReceiveUs = capture_us(c),
                                            .senderFrameTimestamp = a,
                                            .senderSendTimestamp = b,
                                            .receiveTimestamp = c,
                                            .frameSize = d,
                                            .inputMark = e,
                                            .inputReceived = f};
                IHS_FrameTicket *ticket = NULL;
                IHS_FrameBeginResult result =
                    IHS_FrameTrackerBegin(session->frameTracker, 1, id, &receive, &ticket);
                assert(result == IHS_FrameBeginOK || result == IHS_FrameBeginStale);
                if (ticket) {
                    if (tickets[id])
                        IHS_FrameTicketRelease(ticket);
                    else
                        tickets[id] = ticket;
                }
            }
        } else if (op == 'E') {
            assert(sscanf(line + 1, "%u %u %u", &id, &a, &b) == 3);
            if (tracked) {
                if (tickets[id]) {
                    uint64_t us = capture_us(b ? b : now);
                    if (a == 14 || a == 15)
                        IHS_FrameTicketDecodeStage(tickets[id], a == 15, us);
                    else if (a == 16 || a == 17)
                        IHS_FrameTicketUploadStage(tickets[id], a == 17, us);
                    else
                        assert(false);
                }
            } else
                IHS_SessionReportVideoFrameStage(session, id, (IHS_VideoFrameStage)a, b);
        } else if (op == 'C') {
            assert(sscanf(line + 1, "%u %u %u", &id, &a, &now) == 3);
            if (tracked) {
                assert(a != 0); /* Pending is not a valid public ticket outcome. */
                IHS_FrameOutcome outcome = {.result = (IHS_VideoFrameResult)a,
                                            .completionUs = capture_us(now),
                                            .presentationSerial = a == 1 ? ++presented : 0};
                if (tickets[id])
                    IHS_FrameTicketComplete(tickets[id], &outcome);
            } else
                IHS_SessionReportVideoFrameComplete(session, id, (IHS_VideoFrameResult)a);
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
        } else if (op == 'N') {
            float ping, server, client, link;
            int total, lost;
            assert(sscanf(line + 1, "%f %f %f %f %d %d", &ping, &server, &client, &link, &total,
                          &lost) == 6);
            float values[] = {ping, server * 0.001f, client * 0.001f, link * 0.001f,
                              total ? (float)lost * 100.0f / (float)total : 0};
            IHS_FrameStatsSetNetworkSample(agg, values);
            continue;
        } else if (op == 'F') {
            assert(sscanf(line + 1, "%d", &sendResult) == 1);
            continue;
        } else {
            assert(op == 'S');
            IHS_SessionSendFrameEvents(session, message);
            if (tracked)
                IHS_FrameStatsSettleTracked(agg, session->frameTracker, capture_us(now));
        }
        if (tracked && !batched)
            IHS_FrameStatsCollectTracked(agg, session->frameTracker);
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
        if (frameBody && agg->fullReporting) {
            for (size_t i = 0; i < message->message.n_stats; ++i) {
                const CFrameStats *row = message->message.stats[i];
                uint8_t bytes[512];
                assert(cframe_stats__get_packed_size(row) <= sizeof(bytes));
                size_t size = cframe_stats__pack(row, bytes);
                printf("%s\"", i ? "," : "");
                hex(bytes, size);
                putchar('"');
            }
        }
        printf("],\"frames\":[%s%s%s]}\n", frameBody ? "\"" : "", frameBody ? frameBody : "",
               frameBody ? "\"" : "");
        free(frameBody);
        frameBody = NULL;
    }
    free(message);
    IHS_SessionDestroy(session);
    IHS_Quit();
    if (tickets) {
        for (unsigned i = 0; i < 65536; ++i)
            if (tickets[i])
                IHS_FrameTicketRelease(tickets[i]);
        free(tickets);
    }
    return 0;
}
