/* Official client vectors drive actual control FIFO, parser, fallback, player prepare,
 * start/stop and pending replay. OS video worker Start fails deliberately so the
 * production complete-frame FIFO can be observed without consuming its contents. */
#include "ihs_buffer.h"
#include "ihs_buffer_ext.h"
#include "ihslib.h"
#include "session/channels/ch_control.h"
#include "session/channels/ch_data.h"
#include "session/channels/video/ch_data_video.h"
#include "session/reliable.h"
#include "session/session_pri.h"
#include "session/session_queue.h"
#include "session/unreliable.h"
#include "test_session.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Private FIFO storage observer; production allocation/replay/purge execute. */
struct IHS_FECFrameNode {
    IHS_SessionFrame frame;
    struct IHS_FECFrameNode *next;
};

static pthread_t caller;
static bool playerOK, backendOK;
static unsigned prepareCount, sizes[16][2], eventCount;
static const char *events[32];
static IHS_StreamVideoConfig current;
static char configHex[4096];
static void onCaller(void) {
    assert(pthread_equal(caller, pthread_self()));
}
static void hex(const void *bytes, size_t size) {
    const uint8_t *p = bytes;
    for (size_t i = 0; i < size; ++i)
        printf("%02x", p[i]);
}
uint32_t __wrap_IHS_SessionPacketTimestamp(void) {
    return 1000000;
}
IHS_Thread *__real_IHS_ThreadCreate(IHS_ThreadFunction *, const char *, void *);
IHS_Thread *__wrap_IHS_ThreadCreate(IHS_ThreadFunction *fn, const char *name, void *ctx) {
    if (!strcmp(name, "IHSVideo")) {
        onCaller();
        return NULL;
    }
    return __real_IHS_ThreadCreate(fn, name, ctx);
}
static int prepare(IHS_Session *s, const IHS_StreamVideoConfig *c, void *ctx) {
    (void)s;
    (void)ctx;
    onCaller();
    assert(prepareCount < 16);
    sizes[prepareCount][0] = c->width;
    sizes[prepareCount++][1] = c->height;
    return playerOK ? 0 : -1;
}
static int start(IHS_Session *s, const IHS_StreamVideoConfig *c, void *ctx) {
    (void)s;
    (void)ctx;
    onCaller();
    current = *c;
    assert(c->codecDataLen * 2 < sizeof(configHex));
    for (size_t i = 0; i < c->codecDataLen; ++i)
        snprintf(configHex + i * 2, 3, "%02x", c->codecData[i]);
    configHex[c->codecDataLen * 2] = 0;
    assert(eventCount < 32);
    events[eventCount++] = "start";
    return backendOK ? 0 : 1;
}
static void stop(IHS_Session *s, void *ctx) {
    (void)s;
    (void)ctx;
    onCaller();
    assert(eventCount < 32);
    events[eventCount++] = "stop";
}
bool __real_IHS_SessionSendControlMessage(IHS_Session *, EStreamControlMessage,
                                          const ProtobufCMessage *);
bool __wrap_IHS_SessionSendControlMessage(IHS_Session *s, EStreamControlMessage type,
                                          const ProtobufCMessage *m) {
    if (type != k_EStreamControlVideoDecoderInfo)
        return __real_IHS_SessionSendControlMessage(s, type, m);
    onCaller();
    assert(eventCount < 32);
    events[eventCount++] = "info";
    return true;
}
static void parseHex(IHS_Buffer *body, const char *raw) {
    for (size_t n = 0; strcmp(raw, "-") && raw[n]; n += 2) {
        unsigned byte;
        assert(raw[n + 1] && sscanf(raw + n, "%2x", &byte) == 1);
        IHS_BufferAppendUInt8(body, byte);
    }
}
static void report(IHS_Session *s) {
    IHS_SessionChannelData *d =
        (void *)IHS_SessionChannelForType(s, IHS_SessionChannelTypeDataVideo);
    printf("{\"phase\":%d,\"paused\":%s,\"channel\":%u,\"decoder\":", s->state.connectionState,
           s->transport.clientPaused ? "true" : "false", d ? d->base.id : 0);
    if (!d || d->discardFrames)
        printf("null");
    else
        printf("{\"active\":true,\"backend\":%s,\"codec\":%u,\"size\":[%u,%u],"
               "\"codec_data\":\"%s\"}",
               backendOK ? "true" : "false", current.codec, current.width, current.height,
               configHex);
    printf(",\"pending\":[");
    unsigned count = s->pendingDataCount;
    for (unsigned i = 0; i < count; ++i) {
        IHS_QueueItem *item = IHS_QueuePoll(s->pendingData);
        assert(item);
        IHS_SessionPacket *p = &((QueuedPacket *)item)->packet;
        printf("%s{\"kind\":%u,\"channel\":%u,\"fragment\":%u,\"id\":%u,\"sent\":%u,\"first\":%u,"
               "\"received\":%u,\"body\":\"",
               i ? "," : "", p->header.type, p->header.channelId, p->header.fragmentId,
               p->header.packetId, p->header.sendTimestamp, p->header.receiveBegin,
               p->header.receiveTimestamp);
        hex(IHS_BufferPointer(&p->body), p->body.size);
        printf("\"}");
        IHS_QueueAppend(s->pendingData, item);
    }
    printf("],\"delays\":{");
    bool comma = false;
    for (unsigned n = 3; n < 32; ++n)
        if (s->unreliable[n]) {
            printf("%s\"%u\":%u", comma ? "," : "", n, s->unreliable[n]->maxReceiveDelay);
            comma = true;
        }
    printf("},\"prepares\":[");
    for (unsigned n = 0; n < prepareCount; ++n)
        printf("%s[\"prepare\",%u,%u,%s]", n ? "," : "", sizes[n][0], sizes[n][1],
               playerOK ? "true" : "false");
    printf("],\"lifecycle\":[");
    for (unsigned n = 0; n < eventCount; ++n)
        printf("%s\"%s\"", n ? "," : "", events[n]);
    printf("],\"queued\":[");
    if (d) {
        IHS_MutexLock(d->windowLock);
        unsigned count = 0;
        for (struct IHS_FECFrameNode *node = d->fecHead; node; node = node->next) {
            IHS_SessionFrame *frame = &node->frame;
            IHS_SessionDataFrameHeader h = {0};
            IHS_Buffer data = frame->body;
            IHS_BufferOffsetBy(&data, 1);
            assert(IHS_SessionChannelDataFrameHeaderParse(&h, &data));
            assert(frame->mediaPrepared);
            printf("%s{\"id\":%u,\"timestamp\":%u,\"body\":\"", count ? "," : "", h.id,
                   h.timestamp - frame->mediaOffset);
            hex(IHS_BufferPointer(&data) + 12, data.size - 12);
            printf("\"}");
            ++count;
        }
        assert(count == d->fecQueuedCount);
        IHS_MutexUnlock(d->windowLock);
    }
    puts("]}");
}
int main(int argc, char **argv) {
    assert(argc == 4);
    caller = pthread_self();
    playerOK = atoi(argv[1]);
    backendOK = atoi(argv[2]);
    IHS_Init();
    IHS_Session *s = IHS_TestSessionCreate();
    assert(s);
    IHS_TimerTaskStopImmediate(s->retransmission.timer);
    s->state.connectionState = IHS_SessionConnectionStateConnected;
    s->negotiatedVideoCodec = atoi(argv[3]);
    assert(IHS_SessionUnreliableSetDelay(s, 3, 65));
    IHS_StreamVideoCallbacks callbacks = {.prepare = prepare, .start = start, .stop = stop};
    IHS_SessionSetVideoCallbacks(s, &callbacks, NULL);
    char line[8192], raw[4096];
    while (fgets(line, sizeof(line), stdin)) {
        unsigned kind, channel, id, timestamp, sent, received;
        switch (line[0]) {
        case 'C': {
            assert(sscanf(line, "C %u %4095s", &kind, raw) == 2);
            IHS_Buffer body = IHS_BUFFER_INIT(256, 4096);
            parseHex(&body, raw);
            IHS_SessionPacketHeader header = {.type = IHS_SessionPacketTypeReliable,
                                              .channelId = 1};
            IHS_SessionChannelControlHandoff(s->channels[1], kind, &body, &header);
            IHS_SessionChannelControlDrainIncoming(s->channels[1]);
            IHS_BufferClear(&body, true);
            break;
        }
        case 'M': {
            assert(sscanf(line, "M %u %u %u %u %u %4095s", &channel, &id, &timestamp, &sent,
                          &received, raw) == 6);
            IHS_SessionFrame frame = {.header = {.type = IHS_SessionPacketTypeReliable,
                                                 .channelId = channel,
                                                 .sendTimestamp = sent,
                                                 .receiveTimestamp = received,
                                                 .receiveBegin = received}};
            IHS_BufferInit(&frame.body, 256, 4096);
            IHS_BufferAppendUInt8(&frame.body, k_EStreamDataPacket);
            IHS_BufferAppendUInt16LE(&frame.body, id);
            IHS_BufferAppendUInt32LE(&frame.body, timestamp);
            IHS_BufferAppendUInt16LE(&frame.body, 0);
            IHS_BufferAppendUInt32LE(&frame.body, 0);
            parseHex(&frame.body, raw);
            IHS_SessionDeliverReliableFrame(s, &frame);
            IHS_SessionFrameClear(&frame, true);
            break;
        }
        default:
            assert(false);
        }
        report(s);
    }
    IHS_SessionDestroy(s);

    IHS_Quit();

    return 0;
}
