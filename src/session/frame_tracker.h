#pragma once
#include "ihslib/frame_ticket.h"
#include <stddef.h>

/* Stable allocation block size and ID history horizon; live storage may grow. */
#define IHS_FRAME_TRACKER_CAPACITY 256

typedef struct IHS_FrameTracker IHS_FrameTracker;
typedef enum IHS_FrameBeginResult {
    IHS_FrameBeginOK, IHS_FrameBeginStale, IHS_FrameBeginAmbiguous,
    IHS_FrameBeginCapacity, IHS_FrameBeginClosed, IHS_FrameBeginExhausted
} IHS_FrameBeginResult;
typedef struct IHS_FrameReceive {
    uint64_t firstReceiveUs, lastReceiveUs;
    uint32_t senderFrameTimestamp, senderSendTimestamp, receiveTimestamp;
    uint32_t frameSize;
    uint16_t inputMark;
    uint32_t inputReceived;
} IHS_FrameReceive;
typedef struct IHS_TrackedFrame {
    IHS_FrameIdentity identity;
    IHS_FrameReceive receive;
    uint64_t decodeBeginUs, decodeEndUs;
    bool hasDecodeBegin, hasDecodeEnd;
    uint64_t uploadBeginUs, uploadEndUs;
    bool hasUploadBegin, hasUploadEnd, statsComplete;
    uint64_t completionSerial;
    uint32_t completionTimestamp;
    IHS_FrameOutcome outcome;
} IHS_TrackedFrame;

IHS_FrameTracker *IHS_FrameTrackerCreate(uint64_t sessionId);
/* Strictly increasing epoch. Old records become non-reportable. Calls using
 * the tracker require its active owner reference and exclude concurrent Close. */
bool IHS_FrameTrackerOpenEpoch(IHS_FrameTracker *, uint64_t epoch);
void IHS_FrameTrackerCloseEpoch(IHS_FrameTracker *, uint64_t epoch);
/* Stop/join all tracker consumers first. Drops the active owner reference;
 * outstanding tickets can complete/release after session destruction/IHS_Quit. */
void IHS_FrameTrackerClose(IHS_FrameTracker *);
IHS_FrameBeginResult IHS_FrameTrackerBegin(IHS_FrameTracker *, uint64_t epoch,
    uint16_t frameId, const IHS_FrameReceive *, IHS_FrameTicket **out);
bool IHS_FrameTrackerDecodeStage(IHS_FrameTracker *, IHS_FrameTicket *, bool end, uint64_t us);
size_t IHS_FrameTrackerSettle(IHS_FrameTracker *, uint64_t nowUs, uint64_t timeoutUs,
                             IHS_TrackedFrame *out, size_t capacity);
/* Copy live stages and reserve each READY completion once. Does not settle or
 * expire resources. The collector serializes copy -> protocol application. */
size_t IHS_FrameTrackerCopyStats(IHS_FrameTracker *, IHS_TrackedFrame *, size_t capacity);
/* Allocated storage, including live tickets kept across old epochs. */
size_t IHS_FrameTrackerStorageCapacity(IHS_FrameTracker *);
/* Split publication is internal for deterministic interleaving tests. After a
 * successful Claim, the caller MUST Publish before releasing its reference.
 * Only that claimant may Publish. Public producers use Complete instead. */
bool IHS_FrameTicketClaim(IHS_FrameTicket *);
void IHS_FrameTicketPublish(IHS_FrameTicket *, const IHS_FrameOutcome *);
