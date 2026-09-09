/* Real channel assembly + tracked ownership; the data worker is idle while the
 * test delivers deterministic frame fragments through its actual class hook. */
#include "ihs_buffer.h"
#include "ihs_buffer_ext.h"
#include "ihslib.h"
#include "ihslib/frame_ticket.h"
#include "session/channels/ch_data.h"
#include "session/channels/video/ch_data_video.h"
#include "session/session_pri.h"
#include "test_session.h"
#include <assert.h>
#include <pthread.h>
#include <string.h>
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ready = PTHREAD_COND_INITIALIZER;
static bool initialized;
static unsigned starts, stops, submits;
static IHS_FrameTicket *held[4];
static IHS_VideoEpochInfo active;
static int start(IHS_Session *s, const IHS_VideoEpochInfo *epoch, const IHS_StreamVideoConfig *c,
                 void *ctx) {
    (void)s;
    (void)c;
    (void)ctx;
    active = *epoch;
    ++starts;
    return 0;
}
bool __real_IHS_SessionSendControlMessage(IHS_Session *, EStreamControlMessage,
                                          const ProtobufCMessage *);
bool __wrap_IHS_SessionSendControlMessage(IHS_Session *s, EStreamControlMessage type,
                                          const ProtobufCMessage *m) {
    if (type != k_EStreamControlVideoDecoderInfo)
        return __real_IHS_SessionSendControlMessage(s, type, m);
    pthread_mutex_lock(&lock);
    initialized = true;
    pthread_cond_signal(&ready);
    pthread_mutex_unlock(&lock);
    return true;
}
static IHS_StreamVideoSubmitResult submit(IHS_Session *s, const IHS_VideoEpochInfo *epoch,
                                          uint16_t id, IHS_FrameTicket *ticket, IHS_Buffer *b,
                                          IHS_StreamVideoFrameFlag flags, bool *taken, void *ctx) {
    (void)s;
    (void)flags;
    (void)ctx;
    assert(epoch->session_id == 90 && epoch->video_epoch == active.video_epoch);
    IHS_FrameIdentity identity = IHS_FrameTicketIdentity(ticket);
    assert(identity.frameId == id && identity.epoch == epoch->video_epoch);
    if (!submits)
        assert(id == 65535 && b->size == 3 && !memcmp(IHS_BufferPointer(b), "abc", 3));
    else
        assert(id == 1 && b->size == 1 && *IHS_BufferPointer(b) == 'z');
    assert(submits < 4 && IHS_FrameTicketRetain(ticket));
    held[submits++] = ticket;
    *taken = true;
    return IHS_StreamVideoSubmitOK;
}
static void stop(IHS_Session *s, const IHS_VideoEpochInfo *epoch, void *ctx) {
    (void)s;
    (void)ctx;
    assert(epoch->video_epoch == active.video_epoch);
    ++stops;
}
static void fragment(IHS_SessionChannel *ch, uint16_t id, uint16_t seq, uint8_t flags,
                     char payload) {
    IHS_Buffer b;
    IHS_BufferInit(&b, 8, 8);
    IHS_BufferAppendUInt16LE(&b, seq);
    IHS_BufferAppendUInt8(&b, flags);
    IHS_BufferAppendUInt16LE(&b, 0);
    IHS_BufferAppendUInt16LE(&b, 0);
    IHS_BufferAppendUInt8(&b, payload);
    IHS_SessionDataFrameHeader h = {.id = id};
    ((const IHS_SessionChannelDataClass *)ch->cls)->dataFrame(ch, &h, &b);
    IHS_BufferClear(&b, true);
}
static IHS_SessionChannel *create(IHS_Session *session) {
    initialized = false;
    CStartVideoDataMsg msg = CSTART_VIDEO_DATA_MSG__INIT;
    msg.channel = 3;
    msg.width = 1280;
    msg.height = 720;
    msg.codec = k_EStreamVideoCodecH264;
    IHS_SessionChannel *ch = IHS_SessionChannelDataVideoCreate(session, &msg);
    assert(ch);
    pthread_mutex_lock(&lock);
    while (!initialized)
        pthread_cond_wait(&ready, &lock);
    pthread_mutex_unlock(&lock);
    return ch;
}
int main(void) {
    IHS_Init();
    IHS_Session *session = IHS_TestSessionCreate();
    assert(session);
    assert(IHS_SessionSetVideoTrackingIdentity(session, 90));
    IHS_StreamVideoCallbacks callbacks = {
        .startTracked = start, .submitTracked = submit, .stopTracked = stop};
    IHS_SessionSetVideoCallbacks(session, &callbacks, NULL);
    IHS_SessionChannel *ch = create(session);
    fragment(ch, 65535, 0, VideoFrameFlagKeyFrame, 'a');
    fragment(ch, 65535, 1, 0, 'b');
    fragment(ch, 65535, 2, VideoFrameFlagFrameFinish, 'c');
    assert(submits == 1);
    /* A newer incoming id must never relabel the incomplete older assembly. */
    fragment(ch, 0, 3, 0, 'x');
    fragment(ch, 1, 4, VideoFrameFlagFrameFinish, 'y');
    assert(submits == 1);
    fragment(ch, 1, 5, VideoFrameFlagKeyFrame | VideoFrameFlagFrameFinish, 'z');
    assert(submits == 2);
    IHS_SessionChannelStop(ch);
    IHS_SessionChannelDestroy(ch);
    ch = create(session);
    assert(active.video_epoch == 2);
    fragment(ch, 1, 0, VideoFrameFlagKeyFrame | VideoFrameFlagFrameFinish, 'z');
    IHS_SessionChannelStop(ch);
    IHS_SessionChannelDestroy(ch);
    assert(starts == 2 && stops == 2 && submits == 3);
    IHS_SessionDestroy(session);
    IHS_Quit();
    /* Late GPU-style completion is independent of the destroyed session. */
    for (unsigned i = 0; i < submits; ++i) {
        IHS_FrameOutcome outcome = {.result = IHS_VideoFrameResultDisplayed,
                                    .presentationSerial = i + 1};
        IHS_FrameTicketComplete(held[i], &outcome);
        IHS_FrameTicketRelease(held[i]);
    }
    return 0;
}
