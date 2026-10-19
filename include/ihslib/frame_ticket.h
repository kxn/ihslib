#pragma once
#include "session.h"
#include <stdbool.h>
#include <stdint.h>

/* CPU-only endpoint storage; never contains an IHS_Session or GPU pointer.
 * A ticket reference must be held for every call, including retain. After
 * release the pointer is invalid even if its slot has not yet been reused. */
typedef struct IHS_FrameTicket IHS_FrameTicket;
typedef struct IHS_FrameIdentity {
    uint64_t sessionId, epoch, receiveSerial, slotVersion;
    /* Correlation hint, not task identity or decoder admission. Video input
     * endpoints may have the same hint; receiveSerial distinguishes them. */
    uint64_t wireSequence;
    uint16_t frameId;
} IHS_FrameIdentity;
typedef struct IHS_FrameOutcome {
    IHS_VideoFrameResult result;
    uint64_t completionUs;
    uint64_t uploadBeginUs, uploadEndUs;
    uint64_t presentationSerial, presentationIntervalUs;
    bool hasUpload, hasPresentationInterval;
} IHS_FrameOutcome;

bool IHS_FrameTicketRetain(IHS_FrameTicket *);
void IHS_FrameTicketRelease(IHS_FrameTicket *);
IHS_FrameIdentity IHS_FrameTicketIdentity(const IHS_FrameTicket *);
/* Bounded atomic publication, without acquiring the endpoint/session lock.
 * Exactly one contender wins against completion, expiration, and close. */
bool IHS_FrameTicketComplete(IHS_FrameTicket *, const IHS_FrameOutcome *);

/* Decoder-side only; these take the endpoint short lock. */
bool IHS_FrameTicketDecodeStage(IHS_FrameTicket *, bool end, uint64_t us);
/* Video input submission scope. Begin before the backend call; end at accepted
 * input, with a protocol-side fallback for failure/no backend. End is recorded
 * once per invocation, independently of delayed physical output. */
bool IHS_FrameTicketVideoDecodeStage(IHS_FrameTicket *, bool end);
/* Renderer-side stage capture; short endpoint lock, no session/GPU access. */
bool IHS_FrameTicketUploadStage(IHS_FrameTicket *, bool end, uint64_t us);
void IHS_FrameTicketSetSize(IHS_FrameTicket *, uint32_t bytes);

/* Finished video AU timestamp in the local 16.16 stream-clock domain.
 * Separate from first-RX statistics. Decoder sets it before submission; an
 * output consumer snapshots it while holding its ticket reference. */
void IHS_FrameTicketSetVideoTimestamp(IHS_FrameTicket *, uint32_t timestamp);
bool IHS_FrameTicketVideoTimestamp(IHS_FrameTicket *, uint32_t *timestamp);
