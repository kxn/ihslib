/* Connection-owned unreliable receive state; independent of decoder lifetime. */
#pragma once
#include "channels/channel.h"
#include "fec.h"
#include "ihs_thread.h"
#include "window.h"

typedef struct IHS_UnreliableChannel {
    IHS_SessionChannel base;
    IHS_SessionPacketsWindow *window;
    IHS_FECReceiver *fec;
    IHS_Mutex *lock;
    uint32_t maxReceiveDelay;
    uint32_t lastPacketTimestamp, feedbackDeadline;
    bool haveFeedbackDeadline, feedbackDirty;
} IHS_UnreliableChannel;

/* Get/SetDelay/Received/Update run on the serialized RX protocol owner, or
 * before it starts. Decoder workers never advance or destroy these slots. */
IHS_UnreliableChannel *IHS_SessionUnreliableGet(IHS_Session *, uint8_t, uint16_t);
bool IHS_SessionUnreliableSetDelay(IHS_Session *, uint8_t, uint32_t);
void IHS_SessionUnreliableReceived(IHS_Session *, IHS_SessionPacket *);
void IHS_SessionUnreliableUpdateAt(IHS_Session *, uint8_t, uint32_t);
bool IHS_SessionUnreliableBuildFeedback(IHS_UnreliableChannel *, uint32_t, IHS_SessionPacket *,
                                        uint32_t *);
void IHS_SessionUnreliableDeinit(IHS_Session *);
