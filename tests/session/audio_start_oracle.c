/* Original-client vectors drive real control FIFO/drain, audio factory,
 * pending replay, RX preparation, decoder worker and stop/join. Codec/device
 * callbacks are recording boundaries; this is not a physical SDL audio test. */
#include "ihs_buffer.h"
#include "ihs_buffer_ext.h"
#include "ihslib.h"
#include "session/channels/ch_control.h"
#include "session/channels/ch_data.h"
#include "session/channels/ch_data_audio.h"
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
static bool playerOK = true, startupComplete, failWorker;
static IHS_Thread *audioThread;
static unsigned expected, reported;
static atomic_uint completed;
static char *events[128], *deliveries[128];
static unsigned eventCount;
static void onCaller(void) {
    assert(pthread_equal(caller, pthread_self()));
}
static void hex(const void *bytes, size_t size) {
    const uint8_t *p = bytes;
    for (size_t i = 0; i < size; ++i)
        printf("%02x", p[i]);
}
static FILE *event(void) {
    onCaller();
    assert(eventCount < 128);
    size_t ignored;
    FILE *f = open_memstream(&events[eventCount++], &ignored);
    assert(f);
    return f;
}
static void simple(const char *name) {
    FILE *f = event();
    fprintf(f, "[\"%s\"]", name);
    assert(!fclose(f));
}
uint32_t __wrap_IHS_SessionPacketTimestamp(void) {
    return 1000000;
}
IHS_Thread *__real_IHS_ThreadCreate(IHS_ThreadFunction *, const char *, void *);
IHS_Thread *__wrap_IHS_ThreadCreate(IHS_ThreadFunction *fn, const char *name, void *context) {
    bool audio = !strcmp(name, "IHSAudio");
    if (audio) {
        onCaller();
        assert(!audioThread);
    }
    IHS_Thread *thread = audio && failWorker ? NULL : __real_IHS_ThreadCreate(fn, name, context);
    if (audio)
        audioThread = thread;
    return thread;
}
void __real_IHS_ThreadJoin(IHS_Thread *);
void __wrap_IHS_ThreadJoin(IHS_Thread *thread) {
    __real_IHS_ThreadJoin(thread);
    if (thread == audioThread) {
        onCaller();
        audioThread = NULL;
    }
}
IHS_SessionChannel *__real_IHS_SessionChannelDataAudioCreate(IHS_Session *,
                                                             const CStartAudioDataMsg *);
IHS_SessionChannel *__wrap_IHS_SessionChannelDataAudioCreate(IHS_Session *s,
                                                             const CStartAudioDataMsg *m) {
    FILE *f = event();
    fprintf(f, "[\"configured\",%u,%u,%u,%u,\"", m->channel, m->codec, m->frequency, m->channels);
    for (size_t n = 0; m->has_codec_data && n < m->codec_data.len; ++n)
        fprintf(f, "%02x", m->codec_data.data[n]);
    fprintf(f, "\"]");
    assert(!fclose(f));
    startupComplete = false;
    IHS_SessionChannel *result = __real_IHS_SessionChannelDataAudioCreate(s, m);
    startupComplete = true;
    return result;
}
bool __real_IHS_SessionUnreliableSetDelay(IHS_Session *, uint8_t, uint32_t);
bool __wrap_IHS_SessionUnreliableSetDelay(IHS_Session *s, uint8_t ch, uint32_t delay) {
    FILE *f = event();
    fprintf(f, "[\"delay\",%u,%u]", ch, delay);
    assert(!fclose(f));
    return __real_IHS_SessionUnreliableSetDelay(s, ch, delay);
}
void __real_IHS_SessionDrainPendingData(IHS_Session *, IHS_SessionChannel *);
void __wrap_IHS_SessionDrainPendingData(IHS_Session *s, IHS_SessionChannel *ch) {
    if (ch->type == IHS_SessionChannelTypeDataAudio) {
        assert(startupComplete);
        simple("pending");
    }
    __real_IHS_SessionDrainPendingData(s, ch);
}
void __real_IHS_SessionChannelDataReceiveReliableFrame(IHS_SessionChannel *, IHS_SessionFrame *);
void __wrap_IHS_SessionChannelDataReceiveReliableFrame(IHS_SessionChannel *ch,
                                                       IHS_SessionFrame *frame) {
    IHS_SessionChannelData *d = (void *)ch;
    onCaller();
    assert(frame->mediaPrepared);
    if (!d->interrupted && !d->discardFrames && d->worker)
        ++expected;
    __real_IHS_SessionChannelDataReceiveReliableFrame(ch, frame);
}
static int start(IHS_Session *s, const IHS_StreamAudioConfig *config, void *context) {
    (void)s;
    (void)context;
    assert(!audioThread);
    FILE *f = event();
    fprintf(f, "[\"start\",%u,%u,%u,\"", config->codec, config->frequency, config->channels);
    for (size_t n = 0; n < config->codecDataLen; ++n)
        fprintf(f, "%02x", config->codecData[n]);
    fprintf(f, "\"]");
    assert(!fclose(f));
    if (playerOK)
        simple("ready");
    return playerOK ? 0 : -1;
}
static int submit(IHS_Session *s, const IHS_StreamAudioFrameInfo *info, IHS_Buffer *body,
                  void *context) {
    (void)s;
    (void)context;
    assert(!pthread_equal(caller, pthread_self()));
    unsigned n = atomic_load(&completed);
    assert(n < 128);
    size_t ignored;
    FILE *f = open_memstream(&deliveries[n], &ignored);
    assert(f);
    fprintf(f, "{\"id\":%u,\"timestamp\":%u,\"body\":\"", info->id, info->timestamp);
    for (size_t i = 0; i < body->size; ++i)
        fprintf(f, "%02x", IHS_BufferPointer(body)[i]);
    fprintf(f, "\"}");
    assert(!fclose(f));
    atomic_store(&completed, n + 1);
    return 0;
}
static void stop(IHS_Session *s, void *context) {
    (void)s;
    (void)context;
    assert(!audioThread); /* Join must finish before backend teardown. */
    simple("stop");
}
static void parseHex(IHS_Buffer *body, const char *raw) {
    for (size_t n = 0; strcmp(raw, "-") && raw[n]; n += 2) {
        unsigned byte;
        assert(raw[n + 1] && sscanf(raw + n, "%2x", &byte) == 1);
        IHS_BufferAppendUInt8(body, byte);
    }
}
static void report(IHS_Session *s) {
    /* Expected is counted at the actual queue boundary, never from golden
     * outputs. All accepted callbacks finish before reading/resetting results. */
    for (unsigned i = 0; i < 5000 && atomic_load(&completed) < expected; ++i)
        usleep(1000);
    assert(atomic_load(&completed) == expected);
    IHS_SessionChannelData *d =
        (void *)IHS_SessionChannelForType(s, IHS_SessionChannelTypeDataAudio);
    printf("{\"phase\":%d,\"paused\":%s,\"channel\":%u,\"decoder_ready\":%s,\"pending\":[",
           s->state.connectionState, s->transport.clientPaused ? "true" : "false",
           d ? d->base.id : 0, d && !d->discardFrames ? "true" : "false");
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
    printf("},\"events\":[");
    for (unsigned n = 0; n < eventCount; ++n) {
        printf("%s%s", n ? "," : "", events[n]);
        free(events[n]);
    }
    eventCount = 0;
    printf("],\"received\":[");
    for (unsigned n = reported; n < expected; ++n) {
        printf("%s%s", n > reported ? "," : "", deliveries[n]);
        free(deliveries[n]);
    }
    reported = expected;
    printf("],\"queued\":[");
    if (failWorker && d) {
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
    assert(argc == 2 || argc == 3);
    caller = pthread_self();
    failWorker = argc == 3;
    IHS_Init();
    IHS_Session *s = IHS_TestSessionCreate();
    assert(s);
    IHS_TimerTaskStopImmediate(s->retransmission.timer);
    s->state.connectionState = IHS_SessionConnectionStateConnected;
    s->state.connectionId = 12;
    s->state.hostConnectionId = 34;
    s->negotiatedConfig = cnegotiated_config__unpack(NULL, 0, NULL);
    assert(s->negotiatedConfig);
    s->negotiatedConfig->has_selected_audio_codec = 1;
    s->negotiatedConfig->selected_audio_codec = atoi(argv[1]);
    assert(__real_IHS_SessionUnreliableSetDelay(s, 3, 65));
    IHS_StreamAudioCallbacks callbacks = {.start = start, .stop = stop, .submitFrame = submit};
    IHS_SessionSetAudioCallbacks(s, &callbacks, NULL);
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
        case 'P':
            assert(sscanf(line, "P %u", &kind) == 1);
            playerOK = kind != 0;
            break;
        case 'Q':
            assert(sscanf(line, "Q %u %u", &kind, &channel) == 2);
            s->state.connectionState = kind;
            s->transport.clientPaused = channel != 0;
            break;
        default:
            assert(false);
        }
        report(s);
    }
    IHS_SessionDestroy(s);
    assert(!audioThread);
    IHS_Quit();
    for (unsigned n = 0; n < eventCount; ++n)
        free(events[n]);
    return 0;
}
