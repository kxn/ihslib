#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
#include "ihslib.h"
#include "session/frame_tracker.h"

#ifdef IHS_TEST_ALLOC_FAILURE
static _Thread_local unsigned failAllocation;
void *__real_calloc(size_t, size_t);
void *__wrap_calloc(size_t count, size_t size) {
    if (failAllocation && --failAllocation == 0) return NULL;
    return __real_calloc(count, size);
}
static void allocation_failures(void) {
    failAllocation = 1;
    assert(!IHS_FrameTrackerCreate(1));
    failAllocation = 2; /* endpoint allocated, mutex allocation fails */
    assert(!IHS_FrameTrackerCreate(1));
    failAllocation = 0;
}
#endif

static IHS_FrameTicket *begin(IHS_FrameTracker *t, uint64_t epoch, uint16_t id, uint64_t us) {
    IHS_FrameReceive receive = {.firstReceiveUs = us, .lastReceiveUs = us, .frameSize = 10};
    IHS_FrameTicket *r = NULL;
    assert(IHS_FrameTrackerBegin(t, epoch, id, &receive, &r) == IHS_FrameBeginOK);
    assert(r);
    return r;
}
static IHS_FrameOutcome displayed(uint64_t serial) {
    return (IHS_FrameOutcome){.result = IHS_VideoFrameResultDisplayed,
        .completionUs = 1000, .presentationSerial = serial};
}
static void admitted_input_ownership(void) {
    IHS_FrameTracker *t = IHS_FrameTrackerCreate(9);
    assert(t && IHS_FrameTrackerOpenEpoch(t, 1));
    IHS_FrameReceive receive = {.firstReceiveUs = 100, .lastReceiveUs = 100};
    IHS_FrameTicket *first, *next, *jump, *abandoned;
    assert(IHS_FrameTrackerBeginInput(t, 1, 0, &receive, &first) == IHS_FrameBeginOK);
    IHS_FrameTicketSetVideoTimestamp(first, 1000);
    assert(IHS_FrameTicketVideoDecodeStage(first, false));
    assert(!IHS_FrameTicketAbandonInput(first));
    assert(IHS_FrameTrackerBeginInput(t, 1, 0, &receive, &next) == IHS_FrameBeginOK);
    assert(first != next && IHS_FrameTicketIdentity(first).receiveSerial !=
                                IHS_FrameTicketIdentity(next).receiveSerial);
    IHS_FrameTicketSetVideoTimestamp(next, 2000);
    uint32_t timestamp;
    assert(IHS_FrameTicketVideoTimestamp(first, &timestamp) && timestamp == 1000);
    IHS_FrameOutcome outcome = displayed(1);
    assert(IHS_FrameTicketComplete(first, &outcome));
    assert(!IHS_FrameTicketAbandonInput(first));
    assert(IHS_FrameTrackerBeginInput(t, 1, 32768, &receive, &jump) == IHS_FrameBeginOK);
    assert(IHS_FrameTrackerBeginInput(t, 1, 40000, &receive, &abandoned) == IHS_FrameBeginOK);
    assert(IHS_FrameTicketAbandonInput(abandoned));
    assert(!IHS_FrameTicketComplete(abandoned, &outcome));
    IHS_TrackedFrame frames[8];
    size_t count = IHS_FrameTrackerCopyStats(t, frames, 8);
    assert(count == 3);
    unsigned completed = 0;
    for (size_t n = 0; n < count; ++n) {
        assert(frames[n].identity.frameId != 40000);
        if (frames[n].statsComplete) {
            assert(frames[n].identity.receiveSerial == IHS_FrameTicketIdentity(first).receiveSerial);
            ++completed;
        }
    }
    assert(completed == 1 && IHS_FrameTicketComplete(next, &outcome));
    IHS_FrameTicket *repeated;
    assert(IHS_FrameTrackerBeginInput(t, 1, 0, &receive, &repeated) == IHS_FrameBeginOK);
    assert(repeated != first && repeated != next);
    IHS_FrameTicketRelease(abandoned);
    IHS_FrameTrackerCloseEpoch(t, 1);
    assert(IHS_FrameTrackerBeginInput(t, 1, 0, &receive, &abandoned) == IHS_FrameBeginClosed && !abandoned);
    IHS_FrameTrackerClose(t);
    assert(!IHS_FrameTicketComplete(jump, &outcome));
    IHS_FrameTicketRelease(first);
    IHS_FrameTicketRelease(next);
    IHS_FrameTicketRelease(jump);
    IHS_FrameTicketRelease(repeated);
}
static void identity_and_expiry(void) {
    IHS_FrameTracker *t = IHS_FrameTrackerCreate(1);
    assert(t && IHS_FrameTrackerOpenEpoch(t, 1));
    IHS_FrameTicket *a = begin(t, 1, 65535, 0), *b = begin(t, 1, 0, 1);
    IHS_FrameIdentity ia = IHS_FrameTicketIdentity(a), ib = IHS_FrameTicketIdentity(b);
    assert(ib.wireSequence == ia.wireSequence + 1 && ib.receiveSerial > ia.receiveSerial);
    IHS_FrameTicket *partial = begin(t, 1, 65535, 99);
    assert(partial == a);
    IHS_FrameTicketRelease(partial);
    IHS_FrameReceive receive = {0};
    IHS_FrameTicket *out;
    assert(IHS_FrameTrackerBegin(t, 1, 32768, &receive, &out) == IHS_FrameBeginAmbiguous);
    IHS_TrackedFrame frames[2];
    assert(IHS_FrameTrackerSettle(t, 100, 100, frames, 2) == 1);
    assert(frames[0].identity.receiveSerial == ia.receiveSerial);
    assert(frames[0].receive.firstReceiveUs == 0 && frames[0].receive.lastReceiveUs == 99);
    assert(frames[0].outcome.result == IHS_VideoFrameResultDroppedLate);
    IHS_FrameOutcome result = displayed(1);
    assert(!IHS_FrameTicketComplete(a, &result));
    assert(IHS_FrameTicketComplete(b, &result));
    assert(!IHS_FrameTicketComplete(b, &result));
    assert(IHS_FrameTrackerSettle(t, 100, 100, frames, 2) == 1);
    assert(frames[0].identity.frameId == 0);
    IHS_FrameTicketRelease(a);
    IHS_FrameTicketRelease(b);
    assert(IHS_FrameTrackerBegin(t, 1, 65535, &receive, &out) == IHS_FrameBeginStale);
    assert(IHS_FrameTrackerOpenEpoch(t, 2));
    a = begin(t, 2, 0, 0);
    b = begin(t, 2, 65535, 0); /* reordering before the first observed frame */
    assert(IHS_FrameTicketIdentity(b).wireSequence + 1 == IHS_FrameTicketIdentity(a).wireSequence);
    IHS_FrameTrackerCloseEpoch(t, 1); /* old close must not close epoch 2 */
    assert(IHS_FrameTicketComplete(a, &result));
    IHS_FrameTrackerClose(t);
    assert(!IHS_FrameTicketComplete(b, &result));
    IHS_FrameTicketRelease(a);
    IHS_FrameTicketRelease(b);
}
static void capacity(void) {
    IHS_FrameTracker *t = IHS_FrameTrackerCreate(2);
    assert(IHS_FrameTrackerOpenEpoch(t, 1));
    IHS_FrameTicket *tickets[IHS_FRAME_TRACKER_CAPACITY];
    for (unsigned i = 0; i < IHS_FRAME_TRACKER_CAPACITY; ++i) tickets[i] = begin(t, 1, i, 0);
    IHS_FrameIdentity old = IHS_FrameTicketIdentity(tickets[0]);
    IHS_FrameReceive receive = {0};
    IHS_FrameTicket *extra;
#ifdef IHS_TEST_ALLOC_FAILURE
    failAllocation = 1;
    assert(IHS_FrameTrackerBegin(t, 1, 256, &receive, &extra) == IHS_FrameBeginCapacity);
    assert(!extra && IHS_FrameTrackerStorageCapacity(t) == IHS_FRAME_TRACKER_CAPACITY);
#endif
    extra = begin(t, 1, 256, 0);
    assert(IHS_FrameTrackerStorageCapacity(t) == 2 * IHS_FRAME_TRACKER_CAPACITY);
    for (unsigned i = 0; i < IHS_FRAME_TRACKER_CAPACITY; ++i) {
        assert(extra != tickets[i]);
        assert(IHS_FrameTicketIdentity(tickets[i]).frameId == i);
    }
    IHS_TrackedFrame frames[IHS_FRAME_TRACKER_CAPACITY + 1];
    assert(IHS_FrameTrackerSettle(t, 10, 10, frames, IHS_FRAME_TRACKER_CAPACITY + 1) == IHS_FRAME_TRACKER_CAPACITY + 1);
    IHS_FrameTicket *more = begin(t, 1, 257, 11);
    assert(more != tickets[0]); /* An expired external reference still cannot move/reuse. */
    IHS_FrameTicketRelease(tickets[0]);
    IHS_FrameTicket *next = begin(t, 1, 258, 11);
    IHS_FrameIdentity fresh = IHS_FrameTicketIdentity(next);
    assert(next == tickets[0] && fresh.slotVersion > old.slotVersion && fresh.receiveSerial > old.receiveSerial);
    IHS_FrameTrackerClose(t);
    assert(IHS_FrameTicketIdentity(extra).frameId == 256);
    for (unsigned i = 1; i < IHS_FRAME_TRACKER_CAPACITY; ++i) IHS_FrameTicketRelease(tickets[i]);
    IHS_FrameTicketRelease(extra);
    IHS_FrameTicketRelease(more);
    IHS_FrameTicketRelease(next);
}
static void collected_reuse_requires_an_immutable_snapshot(void) {
    IHS_FrameTracker *t = IHS_FrameTrackerCreate(5);
    assert(t && IHS_FrameTrackerOpenEpoch(t, 1));
    IHS_FrameTicket *tickets[512];
    for (unsigned i = 0; i < 256; ++i) tickets[i] = begin(t, 1, i, 0);
    IHS_FrameIdentity old = IHS_FrameTicketIdentity(tickets[0]);
    IHS_FrameOutcome result = displayed(1);
    assert(IHS_FrameTicketComplete(tickets[0], &result));
    IHS_TrackedFrame *rows = calloc(512, sizeof(*rows));
    assert(rows && IHS_FrameTrackerCopyStats(t, rows, 512) == 256);
    IHS_FrameTicketRelease(tickets[0]);
    tickets[256] = begin(t, 1, 256, 0);
    /* Snapshot was copied while the writer still held a reference. Reuse must
     * wait for another immutable snapshot; grow without moving any live ticket. */
    assert(tickets[256] != tickets[0] && IHS_FrameTrackerStorageCapacity(t) == 512);
    for (unsigned i = 257; i < 512; ++i) tickets[i] = begin(t, 1, i, 0);
    assert(IHS_FrameTrackerCopyStats(t, rows, 512) == 512);
    IHS_FrameTicket *fresh = begin(t, 1, 512, 0);
    assert(fresh == tickets[0] && IHS_FrameTicketIdentity(fresh).slotVersion > old.slotVersion);
    assert(IHS_FrameTrackerStorageCapacity(t) == 512);
    IHS_FrameTicket *overflow = begin(t, 1, 513, 0);
    assert(IHS_FrameTrackerStorageCapacity(t) == 768);
    for (unsigned i = 1; i < 512; ++i)
        assert(IHS_FrameTicketIdentity(tickets[i]).frameId == i);
    IHS_FrameTrackerClose(t);
    for (unsigned i = 1; i < 512; ++i) IHS_FrameTicketRelease(tickets[i]);
    IHS_FrameTicketRelease(fresh);
    assert(IHS_FrameTicketIdentity(overflow).frameId == 513);
    IHS_FrameTicketRelease(overflow);
    free(rows);
}
typedef struct race {
    pthread_barrier_t barrier;
    IHS_FrameTicket *ticket;
} race;
static void *complete_claimed(void *context) {
    race *r = context;
    assert(IHS_FrameTicketClaim(r->ticket));
    pthread_barrier_wait(&r->barrier);
    pthread_barrier_wait(&r->barrier);
    IHS_FrameOutcome result = displayed(1);
    IHS_FrameTicketPublish(r->ticket, &result);
    IHS_FrameTicketRelease(r->ticket);
    return NULL;
}
static void close_while_claimed(void) {
    IHS_FrameTracker *t = IHS_FrameTrackerCreate(3);
    assert(IHS_FrameTrackerOpenEpoch(t, 1));
    race r = {.ticket = begin(t, 1, 1, 0)};
    assert(pthread_barrier_init(&r.barrier, NULL, 2) == 0);
    pthread_t worker;
    assert(pthread_create(&worker, NULL, complete_claimed, &r) == 0);
    pthread_barrier_wait(&r.barrier);
    IHS_TrackedFrame frame;
    assert(IHS_FrameTrackerSettle(t, 10000, 10, &frame, 1) == 0); /* never expire CLAIMED */
    IHS_FrameTrackerClose(t);
    IHS_Quit(); /* no IHS services remain when the final ticket frees endpoint */
    pthread_barrier_wait(&r.barrier);
    assert(pthread_join(worker, NULL) == 0);
    pthread_barrier_destroy(&r.barrier);
}
typedef struct contender {
    pthread_barrier_t *barrier;
    IHS_FrameTicket *ticket;
    uint64_t serial;
    bool won;
} contender;
static void *compete(void *context) {
    contender *c = context;
    pthread_barrier_wait(c->barrier);
    IHS_FrameOutcome result = displayed(c->serial);
    c->won = IHS_FrameTicketComplete(c->ticket, &result);
    IHS_FrameTicketRelease(c->ticket);
    return NULL;
}
static void competing_completions(void) {
    IHS_FrameTracker *t = IHS_FrameTrackerCreate(4);
    assert(t && IHS_FrameTrackerOpenEpoch(t, 1));
    IHS_FrameTicket *ticket = begin(t, 1, 42, 0);
    IHS_FrameOutcome bad = displayed(0);
    assert(!IHS_FrameTicketComplete(ticket, &bad));
    assert(IHS_FrameTrackerDecodeStage(t, ticket, false, 0));
    assert(IHS_FrameTrackerDecodeStage(t, ticket, true, 1));
    pthread_barrier_t barrier;
    assert(pthread_barrier_init(&barrier, NULL, 3) == 0);
    contender c[2] = {{&barrier, ticket, 1, false}, {&barrier, ticket, 2, false}};
    pthread_t workers[2];
    for (int i = 0; i < 2; ++i) {
        assert(IHS_FrameTicketRetain(ticket));
        assert(pthread_create(&workers[i], NULL, compete, &c[i]) == 0);
    }
    pthread_barrier_wait(&barrier);
    for (int i = 0; i < 2; ++i) assert(pthread_join(workers[i], NULL) == 0);
    assert(c[0].won != c[1].won);
    IHS_TrackedFrame frame;
    assert(IHS_FrameTrackerSettle(t, 1000, 10, &frame, 1) == 1);
    assert(frame.outcome.presentationSerial == (c[0].won ? 1 : 2));
    assert(frame.hasDecodeBegin && frame.decodeBeginUs == 0 && frame.hasDecodeEnd);
    assert(IHS_FrameTrackerSettle(t, 1000, 10, &frame, 1) == 0);
    IHS_FrameTicketRelease(ticket);
    pthread_barrier_destroy(&barrier);
    ticket = begin(t, 1, 43, 0);
    IHS_FrameIdentity old = IHS_FrameTicketIdentity(ticket);
    assert(IHS_FrameTicketClaim(ticket));
    assert(IHS_FrameTrackerOpenEpoch(t, 2));
    IHS_FrameTicket *newTicket = begin(t, 2, 43, 0);
    assert(newTicket != ticket); /* old CLAIMED record cannot be recycled */
    IHS_FrameOutcome result = displayed(1);
    IHS_FrameTicketPublish(ticket, &result);
    assert(IHS_FrameTrackerSettle(t, 0, 100, &frame, 1) == 0);
    assert(IHS_FrameTicketIdentity(ticket).receiveSerial == old.receiveSerial);
    assert(IHS_FrameTicketComplete(newTicket, &result));
    assert(IHS_FrameTrackerSettle(t, 0, 100, &frame, 1) == 1 && frame.identity.epoch == 2);
    IHS_FrameTicketRelease(ticket);
    IHS_FrameTrackerClose(t);
    IHS_FrameTicketRelease(newTicket);
}
int main(void) {
    IHS_Init();
#ifdef IHS_TEST_ALLOC_FAILURE
    allocation_failures();
#endif
    identity_and_expiry();
    admitted_input_ownership();
    capacity();
    collected_reuse_requires_an_immutable_snapshot();
    competing_completions();
    close_while_claimed();
    return 0;
}
