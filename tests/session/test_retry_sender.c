/* Ownership during an ACK in send I/O and a concurrently changing RTT.
 * These validate local threading; original instruction vectors test the policy. */
#include "ihs_buffer.h"
#include "ihs_buffer_ext.h"
#include "session/channels/ch_control.h"
#include "session/session_pri.h"
#include "test_session.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>

static void track(IHS_SessionRetransmission *r, unsigned id, bool attempted) {
    IHS_SessionPacket p = {0};
    p.header.type = IHS_SessionPacketTypeReliable;
    p.header.channelId = 1;
    p.header.packetId = id;
    p.header.sendTimestamp = 1;
    IHS_SessionPacketBodyInitialize(&p.body, false);
    IHS_BufferAppendUInt8(&p.body, 7);
    assert(IHS_RetransmissionTrack(r, &p, 0));
    if (attempted)
        IHS_RetransmissionNoteInitialSend(r, &p.header, true, 0);
    IHS_SessionPacketClear(&p, true);
}
typedef struct {
    IHS_SessionRetransmission *r;
    atomic_bool entered, proceed;
    unsigned calls;
    size_t attempts;
} ConcurrentAck;
static bool blocking_send(IHS_SessionPacket *p, void *ctx) {
    ConcurrentAck *c = ctx;
    assert(p->header.packetId == 10);
    c->calls++;
    atomic_store(&c->entered, true);
    while (!atomic_load(&c->proceed))
        sched_yield();
    return true;
}
static void *send_worker(void *ctx) {
    ConcurrentAck *c = ctx;
    c->attempts = IHS_RetransmissionProcessAt(c->r, 25, blocking_send, c);
    return NULL;
}
static void concurrent_ack(bool initial) {
    IHS_SessionRetransmission r;
    IHS_RetransmissionInit(&r, NULL);
    track(&r, 10, !initial);
    track(&r, 11, !initial);
    if (initial)
        IHS_RetransmissionActivate(&r, 1, 10, 2);
    ConcurrentAck c = {.r = &r};
    atomic_init(&c.entered, false);
    atomic_init(&c.proceed, false);
    pthread_t worker;
    assert(!pthread_create(&worker, NULL, send_worker, &c));
    while (!atomic_load(&c.entered))
        sched_yield();
    assert(IHS_RetransmissionAcknowledge(&r, 1, 10, 0, 25));
    assert(IHS_RetransmissionAcknowledge(&r, 1, 11, 0, 25));
    atomic_store(&c.proceed, true);
    assert(!pthread_join(worker, NULL));
    assert(c.attempts == 1 && c.calls == 1 && r.stats.outstanding == 0);
    IHS_RetransmissionDeinit(&r);
}
typedef struct {
    IHS_Session *session;
    atomic_bool ready, go, done;
    uint32_t now;
    unsigned sends;
} ConcurrentClock;
static bool clock_send(IHS_SessionPacket *p, void *ctx) {
    ConcurrentClock *c = ctx;
    p->header.sendTimestamp = c->now;
    c->sends++;
    return true;
}
static void *retry_worker(void *ctx) {
    ConcurrentClock *c = ctx;
    atomic_store(&c->ready, true);
    while (!atomic_load(&c->go))
        sched_yield();
    for (unsigned i = 0; i < 4000; ++i) {
        c->now = 1000 + i * 100;
        IHS_RetransmissionProcessTicks(&c->session->retransmission, i, c->now, clock_send, c);
    }
    atomic_store(&c->done, true);
    return NULL;
}
static void concurrent_clock(void) {
    IHS_Session *s = IHS_TestSessionCreate();
    IHS_TimerTaskStopImmediate(s->retransmission.timer);
    IHS_TimerTaskStopImmediate(((IHS_SessionChannelControl *)s->channels[1])->feedbackTimer);
    for (unsigned id = 100; id < 132; ++id)
        track(&s->retransmission, id, true);
    ConcurrentClock c = {.session = s};
    atomic_init(&c.ready, false);
    atomic_init(&c.go, false);
    atomic_init(&c.done, false);
    pthread_t worker;
    assert(!pthread_create(&worker, NULL, retry_worker, &c));
    while (!atomic_load(&c.ready))
        sched_yield();
    unsigned changes = 1;
    IHS_MutexLock(s->clock.lock);
    s->clock.smoothedRTT = 65;
    IHS_MutexUnlock(s->clock.lock);
    atomic_store(&c.go, true);
    while (!atomic_load(&c.done)) {
        IHS_MutexLock(s->clock.lock);
        s->clock.smoothedRTT = ++changes & 1 ? 65 : 3276;
        IHS_MutexUnlock(s->clock.lock);
        sched_yield();
    }
    assert(!pthread_join(worker, NULL));
    assert(c.sends > 0 && changes > 0);
    IHS_SessionDestroy(s);
}
int main(void) {
    IHS_Init();
    concurrent_ack(false);
    concurrent_ack(true);
    concurrent_clock();
    IHS_Quit();
    return 0;
}
