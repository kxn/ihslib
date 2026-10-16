/* Admission ordering, independent sequence spaces, ring growth and teardown. */
#include "ihs_buffer.h"
#include "ihs_buffer_ext.h"
#include "session/channels/ch_control.h"
#include "session/session_pri.h"
#include "test_session.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <string.h>
typedef struct {
    IHS_Session *s;
    unsigned tag;
    atomic_uint *ready;
    atomic_bool *go;
} Producer;
typedef struct {
    IHS_SessionPacket packet;
    bool reliable;
} Queued;
static void *produce(void *ctx) {
    Producer *p = ctx;
    IHS_SessionChannel ch = {.session = p->s, .id = 7};
    IHS_SessionFrame frame;
    IHS_SessionChannelInitializeFrame(&ch, &frame, IHS_SessionPacketTypeUnreliable, false,
                                      IHS_PACKET_ID_NEXT);
    uint8_t payload[1200];
    memset(payload, p->tag, sizeof(payload));
    IHS_BufferAppendMem(&frame.body, payload, sizeof(payload));
    atomic_fetch_add(p->ready, 1);
    while (!atomic_load(p->go))
        sched_yield();
    assert(IHS_SessionChannelQueueFrame(&ch, &frame, false));
    IHS_SessionFrameClear(&frame, true);
    return NULL;
}
static void admission(IHS_Session *s) {
    s->state.mtu = 512;
    atomic_uint ready;
    atomic_init(&ready, 0);
    atomic_bool go;
    atomic_init(&go, false);
    pthread_t threads[4];
    Producer args[4];
    for (unsigned i = 0; i < 4; ++i) {
        args[i] = (Producer){s, i + 1, &ready, &go};
        assert(!pthread_create(&threads[i], NULL, produce, &args[i]));
    }
    while (atomic_load(&ready) != 4)
        sched_yield();
    atomic_store(&go, true);
    for (unsigned i = 0; i < 4; ++i)
        assert(!pthread_join(threads[i], NULL));
    unsigned mask = 0;
    for (unsigned n = 0; n < 4; ++n) {
        unsigned tag = 0;
        for (unsigned frag = 0; frag < 3; ++frag) {
            Queued *q = (void *)IHS_QueuePoll(s->sendQueue);
            assert(q && !q->reliable);
            assert(q->packet.header.packetId == n * 3 + frag && !q->packet.header.autoPacketId);
            assert(q->packet.header.type == (frag ? 4 : 3));
            assert(q->packet.header.fragmentId == (frag ? (int)frag - 1 : 2));
            unsigned value = *IHS_BufferPointer(&q->packet.body);
            if (!frag) {
                tag = value;
                assert(!(mask & (1u << tag)));
                mask |= 1u << tag;
            }
            assert(value == tag);
            IHS_SessionPacketClear(&q->packet, true);
            IHS_QueueItemFree((void *)q);
        }
    }
    assert(mask == 30 && !IHS_QueuePoll(s->sendQueue));
    IHS_SessionChannel ch = {.session = s, .id = 7};
    // A distinct reliable window begins at zero, while unreliable keeps its ID.
    for (unsigned reliable = 0; reliable < 2; ++reliable) {
        IHS_SessionPacket p;
        IHS_SessionChannelInitializePacket(&ch, &p, reliable ? 5 : 3, false, IHS_PACKET_ID_NEXT);
        IHS_BufferAppendUInt8(&p.body, 7);
        assert(IHS_SessionQueuePacket(s, &p, reliable));
        assert(p.header.packetId == (reliable ? 0 : 12));
        IHS_SessionPacketClear(&p, true);
        Queued *q = (void *)IHS_QueuePoll(s->sendQueue);
        assert(q);
        IHS_SessionPacketClear(&q->packet, true);
        IHS_QueueItemFree((void *)q);
    }
}
static void growth(IHS_Session *s) {
    for (unsigned i = 0; i < 100; ++i) {
        IHS_SessionPacket p = {0};
        p.header.type = 3;
        p.header.channelId = 8;
        p.header.packetId = (uint16_t)(65500 + i);
        IHS_SessionPacketBodyInitialize(&p.body, false);
        IHS_BufferAppendUInt32LE(&p.body, i);
        assert(IHS_OutgoingRetain(s, &p, 1000000 + i));
        IHS_SessionPacketClear(&p, true);
    }
    IHS_OutgoingWindow *w = &s->outgoing.unreliable[8];
    assert(w->capacity == 128 && w->head == 65500 && w->tail == 64);
    for (unsigned i = 0; i < 100; ++i) {
        unsigned slot = (uint16_t)(65500 + i) & 127;
        IHS_OutgoingPacket *p = w->slots[slot];
        assert(p);
        assert(p->packet.header.packetId == (uint16_t)(65500 + i));
        assert(w->created[slot] == 1000000 + i);
        const uint8_t *v = IHS_BufferPointer(&p->packet.body);
        assert((v[0] | (uint32_t)v[1] << 8 | (uint32_t)v[2] << 16 | (uint32_t)v[3] << 24) == i);
    }
}
int main(void) {
    IHS_Init();
    IHS_Session *s = IHS_TestSessionCreate();
    assert(s);
    IHS_TimerTaskStopImmediate(s->retransmission.timer);
    IHS_TimerTaskStopImmediate(((IHS_SessionChannelControl *)s->channels[1])->feedbackTimer);
    admission(s);
    growth(s);
    IHS_SessionDestroy(s);
    IHS_Quit();
    return 0;
}
