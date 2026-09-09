#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "session.h"

/* CPU-only endpoint storage; never contains an IHS_Session or GPU pointer.
 * A ticket reference must be held for every call, including retain. After
 * release the pointer is invalid even if its slot has not yet been reused. */
typedef struct IHS_FrameTicket IHS_FrameTicket;
typedef struct IHS_FrameIdentity {
    uint64_t sessionId, epoch, receiveSerial, slotVersion;
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
