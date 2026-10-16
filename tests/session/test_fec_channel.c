/* Raw datagram -> session pending-data routing -> real media worker. */
#include "ihs_buffer.h"
#include "ihs_buffer_ext.h"
#include "session/channels/ch_control.h"
#include "session/channels/ch_data.h"
#include "test_session.h"
#include <assert.h>
#include <pthread.h>
#include <string.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ready = PTHREAD_COND_INITIALIZER;
static bool entered, released, final;
static unsigned delivered;

static void init(IHS_SessionChannel *c, const void *cfg) {
    (void)cfg;
    IHS_SessionChannelDataInit(c, 32);
}
static bool start(IHS_SessionChannel *c) {
    (void)c;
    return true;
}
static void stop(IHS_SessionChannel *c) {
    (void)c;
}
static void data(IHS_SessionChannel *c, const IHS_SessionDataFrameHeader *h, IHS_Buffer *b) {
    (void)c;
    assert(b->size == 3 && !memcmp(IHS_BufferPointer(b), "fec", 3));
    pthread_mutex_lock(&lock);
    delivered++;
    if (!entered) {
        assert(h->id == 10); /* early group, released when channel is added */
        entered = true;
        pthread_cond_broadcast(&ready);
        while (!released)
            pthread_cond_wait(&ready, &lock);
    }
    if (h->id == 1000) {
        final = true;
        pthread_cond_broadcast(&ready);
    }
    pthread_mutex_unlock(&lock);
}
static const IHS_SessionChannelDataClass cls = {
    .base = {.init = init,
             .deinit = IHS_SessionChannelDataDeinit,
             .received = IHS_SessionChannelDataReceived,
             .stopped = IHS_SessionChannelDataStopped,
             .instanceSize = sizeof(IHS_SessionChannelData)},
    .start = start,
    .dataFrame = data,
    .stop = stop,
};
static void receive(IHS_Session *s, bool fec, uint16_t group, uint16_t mediaId) {
    IHS_SessionPacket packet;
    IHS_SessionChannel temporary = {.session = s, .id = 3};
    assert(IHS_SessionChannelInitializePacket(
        &temporary, &packet,
        fec ? IHS_SessionPacketTypeUnreliableFEC : IHS_SessionPacketTypeUnreliable, true, group));
    packet.header.srcConnectionId = s->state.hostConnectionId;
    packet.header.dstConnectionId = s->state.connectionId;
    if (fec) {
        IHS_BufferAppendUInt32LE(&packet.body, 16);
        IHS_BufferAppendUInt16LE(&packet.body, 32);
        IHS_BufferAppendUInt16LE(&packet.body, 0);
    }
    IHS_BufferAppendUInt8(&packet.body, k_EStreamDataPacket);
    IHS_BufferAppendUInt16LE(&packet.body, mediaId);
    IHS_BufferAppendUInt32LE(&packet.body, 1234);
    IHS_BufferAppendUInt16LE(&packet.body, 0);
    IHS_BufferAppendUInt32LE(&packet.body, 0);
    IHS_BufferAppendMem(&packet.body, (const uint8_t *)"fec", 3);
    if (fec)
        IHS_BufferFillMem(&packet.body, packet.body.size, 0, 16);
    IHS_SessionPacketPopulateBuffer(&packet);
    IHS_Buffer wire = packet.body;
    memset(&packet.body, 0, sizeof(packet.body));
    IHS_BufferExtendSize(&wire);
    s->base.callbacks.received(&s->base, &s->info.address, &wire);
    IHS_BufferClear(&wire, true);
}
int main(void) {
    IHS_Init();
    IHS_Session *s = IHS_TestSessionCreate();
    assert(s);
    IHS_TimerTaskStopImmediate(s->retransmission.timer);
    receive(s, true, 0, 10);
    assert(s->pendingDataCount == 1);
    IHS_SessionChannel *c =
        IHS_SessionChannelDataCreate(&cls, s, IHS_SessionChannelTypeDataVideo, 3, NULL);
    assert(c);
    IHS_SessionChannelAdd(s, c);
    assert(!s->pendingDataCount);
    pthread_mutex_lock(&lock);
    while (!entered)
        pthread_cond_wait(&ready, &lock);
    pthread_mutex_unlock(&lock);
    for (unsigned i = 1; i <= 70; ++i)
        receive(s, true, i, (uint16_t)(10 + i));
    IHS_SessionChannelData *d = (void *)c;
    IHS_MutexLock(d->windowLock);
    assert(!IHS_SessionPacketsWindowSize(d->window));
    assert(!d->feedbackDirty && !d->haveFeedbackDeadline);
    assert(d->fecQueuedCount == 32 && d->fecQueuedBytes <= d->fecQueueLimit);
    IHS_MutexUnlock(d->windowLock);
    IHS_MutexLock(s->sendQueueMutex);
    assert(IHS_QueueIsEmpty(s->sendQueue)); /* no NACKs for FEC */
    IHS_MutexUnlock(s->sendQueueMutex);
    receive(s, false, 0, 1000); /* FEC IDs did not advance ordinary sequence */
    pthread_mutex_lock(&lock);
    released = true;
    pthread_cond_broadcast(&ready);
    while (!final)
        pthread_cond_wait(&ready, &lock);
    assert(delivered == 34); /* first + bounded last 32 + ordinary */
    pthread_mutex_unlock(&lock);
    IHS_SessionChannelRemove(s, 3); /* stops/joins before FIFO/RS buffers die */
    IHS_SessionDestroy(s);
    IHS_Quit();
    return 0;
}
