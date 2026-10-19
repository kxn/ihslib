#include "frame_tracker.h"
#include "ihs_thread.h"
#include "packet.h"
#include <assert.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <time.h>

enum publication { OPEN, CLAIMED, READY, EXPIRED, CLOSED };
struct IHS_FrameTicket {
    IHS_FrameTracker *owner;
    atomic_uint refs;
    atomic_int publication;
    bool occupied, settled, input;
    IHS_TrackedFrame frame;   /* receive/decode under owner lock; identity immutable */
    IHS_FrameOutcome payload; /* claimant only, readable after READY(acquire) */
    uint64_t completionSerial;
    uint32_t completionTimestamp;
    bool statsConsumed; /* owner lock */
    bool statsReusable; /* completed snapshot observed with no external refs */
    bool hasVideoTimestamp; /* owner lock, independent of receive statistics */
    uint32_t videoTimestamp;
    bool videoDecodeActive, videoDecodeEnded; /* owner lock */
};
typedef struct IHS_FrameTicketBlock {
    struct IHS_FrameTicketBlock *next;
    IHS_FrameTicket records[IHS_FRAME_TRACKER_CAPACITY];
} IHS_FrameTicketBlock;
struct IHS_FrameTracker {
    IHS_Mutex *lock;
    atomic_uint refs;
    atomic_uint_fast64_t completions;
    uint64_t sessionId, epoch, serial, highestSequence;
    bool accepting, haveSequence, statsTracking;
    uint64_t seen[IHS_FRAME_TRACKER_CAPACITY];
    bool seenValid[IHS_FRAME_TRACKER_CAPACITY];
    IHS_FrameTicketBlock first, *last;
    size_t storageCapacity;
};
typedef struct {
    IHS_FrameTicketBlock *block;
    size_t index;
} RecordIterator;
static IHS_FrameTicket *next_record(RecordIterator *it) {
    if (!it->block) return NULL;
    IHS_FrameTicket *r = &it->block->records[it->index++];
    if (it->index == IHS_FRAME_TRACKER_CAPACITY) {
        it->block = it->block->next;
        it->index = 0;
    }
    return r;
}
static void init_block(IHS_FrameTracker *t, IHS_FrameTicketBlock *block) {
    for (size_t i = 0; i < IHS_FRAME_TRACKER_CAPACITY; ++i) {
        block->records[i].owner = t;
        atomic_init(&block->records[i].refs, 0);
        atomic_init(&block->records[i].publication, CLOSED);
    }
}
static bool retain_ref(atomic_uint *ref) {
    unsigned n = atomic_load_explicit(ref, memory_order_relaxed);
    while (n && n != UINT_MAX) {
        if (atomic_compare_exchange_weak_explicit(ref, &n, n + 1, memory_order_relaxed,
                                                  memory_order_relaxed))
            return true;
    }
    return false;
}
static void release_endpoint(IHS_FrameTracker *t) {
    if (atomic_fetch_sub_explicit(&t->refs, 1, memory_order_acq_rel) == 1) {
        IHS_FrameTicketBlock *block = t->first.next;
        while (block) {
            IHS_FrameTicketBlock *next = block->next;
            free(block);
            block = next;
        }
        IHS_MutexDestroy(t->lock);
        free(t);
    }
}
IHS_FrameTracker *IHS_FrameTrackerCreate(uint64_t sessionId) {
    if (!sessionId)
        return NULL;
    IHS_FrameTracker *t = calloc(1, sizeof(*t));
    if (!t)
        return NULL;
    t->lock = IHS_MutexCreate();
    if (!t->lock) {
        free(t);
        return NULL;
    }
    atomic_init(&t->refs, 1);
    atomic_init(&t->completions, 0);
    t->sessionId = sessionId;
    init_block(t, &t->first);
    t->last = &t->first;
    t->storageCapacity = IHS_FRAME_TRACKER_CAPACITY;
    return t;
}
static void close_epoch(IHS_FrameTracker *t) {
    t->accepting = false;
    RecordIterator it = {.block = &t->first};
    IHS_FrameTicket *r;
    while ((r = next_record(&it))) {
        if (!r->occupied)
            continue;
        int state = OPEN;
        atomic_compare_exchange_strong_explicit(&r->publication, &state, CLOSED,
                                                memory_order_acq_rel, memory_order_acquire);
        r->settled = true; /* no further reporting, even if CLAIMED publishes */
    }
}
bool IHS_FrameTrackerOpenEpoch(IHS_FrameTracker *t, uint64_t epoch) {
    IHS_MutexLock(t->lock);
    bool ok = epoch && epoch > t->epoch;
    if (ok) {
        close_epoch(t);
        t->epoch = epoch;
        t->accepting = true;
        t->haveSequence = false;
        for (size_t i = 0; i < IHS_FRAME_TRACKER_CAPACITY; ++i)
            t->seenValid[i] = false;
    }
    IHS_MutexUnlock(t->lock);
    return ok;
}
void IHS_FrameTrackerCloseEpoch(IHS_FrameTracker *t, uint64_t epoch) {
    IHS_MutexLock(t->lock);
    if (epoch == t->epoch)
        close_epoch(t);
    IHS_MutexUnlock(t->lock);
}
void IHS_FrameTrackerClose(IHS_FrameTracker *t) {
    IHS_MutexLock(t->lock);
    close_epoch(t);
    IHS_MutexUnlock(t->lock);
    release_endpoint(t);
}
bool IHS_FrameTicketRetain(IHS_FrameTicket *r) {
    if (!retain_ref(&r->owner->refs))
        return false;
    if (retain_ref(&r->refs))
        return true;
    release_endpoint(r->owner);
    return false;
}
void IHS_FrameTicketRelease(IHS_FrameTicket *r) {
    IHS_FrameTracker *t = r->owner;
    unsigned old = atomic_fetch_sub_explicit(&r->refs, 1, memory_order_acq_rel);
    assert(old != 0);
    (void)old;
    /* A consumer can reuse r after refs reaches zero. Do not access r again. */
    release_endpoint(t);
}
IHS_FrameIdentity IHS_FrameTicketIdentity(const IHS_FrameTicket *r) {
    return r->frame.identity;
}
bool IHS_FrameTicketClaim(IHS_FrameTicket *r) {
    int state = OPEN;
    return atomic_compare_exchange_strong_explicit(&r->publication, &state, CLAIMED,
                                                   memory_order_acq_rel, memory_order_acquire);
}
void IHS_FrameTicketPublish(IHS_FrameTicket *r, const IHS_FrameOutcome *outcome) {
    assert(atomic_load_explicit(&r->publication, memory_order_relaxed) == CLAIMED);
    r->payload = *outcome;
    /* This clock is a pure OS call and remains valid after IHS_Quit. */
    r->completionTimestamp = IHS_SessionPacketTimestamp();
    r->completionSerial = atomic_fetch_add_explicit(&r->owner->completions, 1, memory_order_relaxed);
    atomic_store_explicit(&r->publication, READY, memory_order_release);
}
bool IHS_FrameTicketComplete(IHS_FrameTicket *r, const IHS_FrameOutcome *outcome) {
    if (!outcome || outcome->result <= IHS_VideoFrameResultPending ||
        outcome->result > IHS_VideoFrameResultDroppedReset ||
        (outcome->hasUpload && (outcome->uploadEndUs < outcome->uploadBeginUs ||
                                outcome->completionUs < outcome->uploadEndUs)) ||
        (outcome->result != IHS_VideoFrameResultDisplayed &&
         (outcome->presentationSerial || outcome->hasPresentationInterval)) ||
        (outcome->result == IHS_VideoFrameResultDisplayed && !outcome->presentationSerial))
        return false;
    if (!IHS_FrameTicketClaim(r))
        return false;
    IHS_FrameTicketPublish(r, outcome);
    return true;
}
static IHS_FrameBeginResult begin_frame(IHS_FrameTracker *t, uint64_t epoch, uint16_t frameId,
                                      const IHS_FrameReceive *receive, IHS_FrameTicket **out,
                                      bool input) {
    *out = NULL;
    IHS_MutexLock(t->lock);
    IHS_FrameBeginResult result = IHS_FrameBeginOK;
    if (!t->accepting || epoch != t->epoch) {
        result = IHS_FrameBeginClosed;
        goto done;
    }
    /* Legacy aggregation leaves headroom for reordering before the first ID.
     * Fresh input endpoints keep only a raw-ID hint, never a history gate. */
    uint64_t seq = 65536u + (uint64_t)frameId;
    if (!input && t->haveSequence) {
        uint16_t delta = (uint16_t)(frameId - (uint16_t)t->highestSequence);
        if (delta == 32768) {
            result = IHS_FrameBeginAmbiguous;
            goto done;
        }
        if (delta < 32768) {
            if (UINT64_MAX - t->highestSequence < delta) {
                result = IHS_FrameBeginExhausted;
                goto done;
            }
            seq = t->highestSequence + delta;
        } else {
            unsigned back = 65536u - delta;
            if (back >= IHS_FRAME_TRACKER_CAPACITY || back > t->highestSequence) {
                result = IHS_FrameBeginStale;
                goto done;
            }
            seq = t->highestSequence - back;
        }
    }
    IHS_FrameTicket *freeRecord = NULL, *collectedRecord = NULL;
    RecordIterator it = {.block = &t->first};
    IHS_FrameTicket *r;
    while ((r = next_record(&it))) {
        if (!input && !r->input && r->occupied && r->frame.identity.epoch == epoch &&
            r->frame.identity.wireSequence == seq) {
            if (r->settled || atomic_load_explicit(&r->publication, memory_order_acquire) != OPEN) {
                result = IHS_FrameBeginStale;
                goto done;
            }
            if (!retain_ref(&t->refs)) {
                result = IHS_FrameBeginExhausted;
                goto done;
            }
            /* Owner lock permits acquiring an OPEN record with zero external refs. */
            unsigned refs = atomic_load_explicit(&r->refs, memory_order_relaxed);
            do {
                if (refs == UINT_MAX) {
                    release_endpoint(t);
                    result = IHS_FrameBeginExhausted;
                    goto done;
                }
            } while (!atomic_compare_exchange_weak_explicit(
                &r->refs, &refs, refs + 1, memory_order_relaxed, memory_order_relaxed));
            if (receive->lastReceiveUs >= r->frame.receive.lastReceiveUs) {
                r->frame.receive.lastReceiveUs = receive->lastReceiveUs;
                r->frame.receive.receiveTimestamp = receive->receiveTimestamp;
            }
            *out = r;
            goto done;
        }
        if (!freeRecord && (!r->occupied || r->settled) &&
            atomic_load_explicit(&r->refs, memory_order_acquire) == 0 &&
            r->frame.identity.slotVersion != UINT64_MAX)
            freeRecord = r;
        /* CopyStats may only grant reuse when READY and refs==0 together:
         * no producer can then append a late stage to that copied snapshot.
         * Prefer unused/settled records so ordinary observer history remains
         * available until actual pressure, as before. */
        if (!collectedRecord && r->occupied && !r->settled &&
            r->frame.identity.slotVersion != UINT64_MAX &&
            atomic_load_explicit(&r->refs, memory_order_acquire) == 0 &&
            atomic_load_explicit(&r->publication, memory_order_acquire) == READY &&
            (!t->statsTracking || (r->statsConsumed && r->statsReusable)))
            collectedRecord = r;
    }
    size_t seenIndex = seq % IHS_FRAME_TRACKER_CAPACITY;
    if (!input && t->seenValid[seenIndex] && t->seen[seenIndex] == seq) {
        result = IHS_FrameBeginStale;
        goto done;
    }
    if (!freeRecord) freeRecord = collectedRecord;
    if (!freeRecord) {
        if (t->storageCapacity > SIZE_MAX - IHS_FRAME_TRACKER_CAPACITY) {
            result = IHS_FrameBeginExhausted;
            goto done;
        }
        IHS_FrameTicketBlock *block = calloc(1, sizeof(*block));
        if (!block) {
            result = IHS_FrameBeginCapacity; /* Actual allocation failure. */
            goto done;
        }
        init_block(t, block);
        t->last->next = block;
        t->last = block;
        t->storageCapacity += IHS_FRAME_TRACKER_CAPACITY;
        freeRecord = &block->records[0];
    }
    if (t->serial == UINT64_MAX || !retain_ref(&t->refs)) {
        result = IHS_FrameBeginExhausted;
        goto done;
    }
    uint64_t version = freeRecord->frame.identity.slotVersion + 1;
    freeRecord->frame = (IHS_TrackedFrame){
        .identity = {t->sessionId, epoch, ++t->serial, version, seq, frameId}, .receive = *receive};
    freeRecord->hasVideoTimestamp = false;
    freeRecord->videoTimestamp = 0;
    freeRecord->videoDecodeActive = freeRecord->videoDecodeEnded = false;
    freeRecord->occupied = true;
    freeRecord->input = input;
    freeRecord->settled = false;
    freeRecord->statsConsumed = false;
    freeRecord->statsReusable = false;
    atomic_store_explicit(&freeRecord->refs, 1, memory_order_relaxed);
    atomic_store_explicit(&freeRecord->publication, OPEN, memory_order_release);
    if (!input) {
        t->seenValid[seenIndex] = true;
        t->seen[seenIndex] = seq;
        if (!t->haveSequence || seq > t->highestSequence)
            t->highestSequence = seq;
        t->haveSequence = true;
    }
    *out = freeRecord;
done:
    IHS_MutexUnlock(t->lock);
    return result;
}
IHS_FrameBeginResult IHS_FrameTrackerBegin(IHS_FrameTracker *t, uint64_t epoch, uint16_t frameId,
                                           const IHS_FrameReceive *receive, IHS_FrameTicket **out) {
    return begin_frame(t, epoch, frameId, receive, out, false);
}
IHS_FrameBeginResult IHS_FrameTrackerBeginInput(IHS_FrameTracker *t, uint64_t epoch, uint16_t frameId,
                                                const IHS_FrameReceive *receive, IHS_FrameTicket **out) {
    return begin_frame(t, epoch, frameId, receive, out, true);
}
bool IHS_FrameTicketAbandonInput(IHS_FrameTicket *r) {
    IHS_FrameTracker *t = r->owner;
    IHS_MutexLock(t->lock);
    int state = OPEN;
    bool retired = r->input && !r->settled && !r->videoDecodeActive && !r->frame.hasDecodeBegin &&
                   atomic_compare_exchange_strong_explicit(&r->publication, &state, CLOSED,
                                                            memory_order_acq_rel, memory_order_acquire);
    if (retired) r->settled = true;
    IHS_MutexUnlock(t->lock);
    return retired;
}
bool IHS_FrameTrackerDecodeStage(IHS_FrameTracker *t, IHS_FrameTicket *r, bool end, uint64_t us) {
    IHS_MutexLock(t->lock);
    bool ok = r->owner == t && t->accepting && r->frame.identity.epoch == t->epoch && !r->settled;
    if (ok) {
        if (end) {
            r->frame.decodeEndUs = us;
            r->frame.hasDecodeEnd = true;
        } else if (!r->frame.hasDecodeBegin) {
            r->frame.decodeBeginUs = us;
            r->frame.hasDecodeBegin = true;
        }
    }
    IHS_MutexUnlock(t->lock);
    return ok;
}
bool IHS_FrameTicketUploadStage(IHS_FrameTicket *r, bool end, uint64_t us) {
    IHS_FrameTracker *t = r->owner;
    IHS_MutexLock(t->lock);
    bool ok = t->accepting && r->frame.identity.epoch == t->epoch && !r->settled;
    if (ok) {
        if (end) {
            r->frame.uploadEndUs = us;
            r->frame.hasUploadEnd = true;
        } else {
            r->frame.uploadBeginUs = us;
            r->frame.hasUploadBegin = true;
        }
    }
    IHS_MutexUnlock(t->lock);
    return ok;
}

size_t IHS_FrameTrackerCopyStats(IHS_FrameTracker *t, IHS_TrackedFrame *out, size_t capacity) {
    size_t count = 0;
    IHS_MutexLock(t->lock);
    t->statsTracking = true;
    if (t->accepting) {
        RecordIterator it = {.block = &t->first};
        IHS_FrameTicket *r;
        while (count < capacity && (r = next_record(&it))) {
            if (!r->occupied || r->settled || r->frame.identity.epoch != t->epoch)
                continue;
            out[count] = r->frame;
            if (atomic_load_explicit(&r->publication, memory_order_acquire) == READY && !r->statsConsumed) {
                out[count].statsComplete = true;
                out[count].outcome = r->payload;
                out[count].completionSerial = r->completionSerial;
                out[count].completionTimestamp = r->completionTimestamp;
                r->statsConsumed = true;
            }
            if (atomic_load_explicit(&r->publication, memory_order_acquire) == READY &&
                atomic_load_explicit(&r->refs, memory_order_acquire) == 0)
                r->statsReusable = true;
            ++count;
        }
    }
    IHS_MutexUnlock(t->lock);
    return count;
}
size_t IHS_FrameTrackerStorageCapacity(IHS_FrameTracker *t) {
    IHS_MutexLock(t->lock);
    size_t capacity = t->storageCapacity;
    IHS_MutexUnlock(t->lock);
    return capacity;
}
size_t IHS_FrameTrackerSettle(IHS_FrameTracker *t, uint64_t nowUs, uint64_t timeoutUs,
                              IHS_TrackedFrame *out, size_t capacity) {
    size_t count = 0;
    IHS_MutexLock(t->lock);
    RecordIterator it = {.block = &t->first};
    IHS_FrameTicket *r;
    while (count < capacity && (r = next_record(&it))) {
        if (!r->occupied || r->settled)
            continue;
        int state = atomic_load_explicit(&r->publication, memory_order_acquire);
        if (state == OPEN && nowUs >= r->frame.receive.firstReceiveUs &&
            nowUs - r->frame.receive.firstReceiveUs >= timeoutUs) {
            if (atomic_compare_exchange_strong_explicit(&r->publication, &state, EXPIRED,
                                                        memory_order_acq_rel, memory_order_acquire))
                state = EXPIRED;
        }
        if (state != READY && state != EXPIRED)
            continue;
        /* A publication racing between protocol collection and this cleanup
         * must remain available for the next collection. */
        if (state == READY && t->statsTracking && !r->statsConsumed)
            continue;
        out[count] = r->frame;
        out[count++].outcome = state == READY
                                   ? r->payload
                                   : (IHS_FrameOutcome){.result = IHS_VideoFrameResultDroppedLate,
                                                        .completionUs = nowUs};
        r->settled = true;
    }
    IHS_MutexUnlock(t->lock);
    return count;
}

bool IHS_FrameTicketDecodeStage(IHS_FrameTicket *r, bool end, uint64_t us) {
    return IHS_FrameTrackerDecodeStage(r->owner, r, end, us);
}
bool IHS_FrameTicketVideoDecodeStage(IHS_FrameTicket *r, bool end) {
    IHS_FrameTracker *t = r->owner;
    IHS_MutexLock(t->lock);
    bool ok = t->accepting && r->frame.identity.epoch == t->epoch && !r->settled;
    if (end) ok = ok && r->videoDecodeActive && !r->videoDecodeEnded;
    if (ok) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        uint64_t us = (uint64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000;
        if (end) {
            r->frame.decodeEndUs = us;
            r->frame.hasDecodeEnd = r->videoDecodeEnded = true;
        } else {
            r->videoDecodeActive = true;
            r->videoDecodeEnded = false;
            if (!r->frame.hasDecodeBegin) {
                r->frame.decodeBeginUs = us;
                r->frame.hasDecodeBegin = true;
            }
        }
    }
    IHS_MutexUnlock(t->lock);
    return ok;
}
void IHS_FrameTicketSetSize(IHS_FrameTicket *r, uint32_t bytes) {
    IHS_FrameTracker *t = r->owner;
    IHS_MutexLock(t->lock);
    if (!r->settled)
        r->frame.receive.frameSize = bytes;
    IHS_MutexUnlock(t->lock);
}

void IHS_FrameTicketSetVideoTimestamp(IHS_FrameTicket *r, uint32_t timestamp) {
    IHS_FrameTracker *t = r->owner;
    IHS_MutexLock(t->lock);
    if (t->accepting && r->frame.identity.epoch == t->epoch && !r->settled) {
        r->videoTimestamp = timestamp;
        r->hasVideoTimestamp = true;
    }
    IHS_MutexUnlock(t->lock);
}
bool IHS_FrameTicketVideoTimestamp(IHS_FrameTicket *r, uint32_t *timestamp) {
    IHS_FrameTracker *t = r->owner;
    IHS_MutexLock(t->lock);
    bool have = r->hasVideoTimestamp;
    if (have && timestamp) *timestamp = r->videoTimestamp;
    IHS_MutexUnlock(t->lock);
    return have;
}
