/* Official client OnStreamPacket 0x7acfa8 -> CStreamQueue ->
 * HandleIncomingPackets 0x7a6368. Protocol reference §4/28/46/50.
 * Decryption and sequence advancement happen before this handoff. */
#include "ch_control.h"
#include "session/session_pri.h"
#include <stdlib.h>

void IHS_SessionChannelControlHandoff(IHS_SessionChannel *channel, EStreamControlMessage type,
                                      IHS_Buffer *body, const IHS_SessionPacketHeader *header) {
    if (type == k_EStreamControlKeepAlive)
        return;
    if (type == k_EStreamControlRemoteHID) {
        IHS_SessionChannelControlOnMessageReceived(channel, type, body, header);
        return;
    }
    IHS_IncomingControl *message = calloc(1, sizeof(*message));
    if (!message) {
        /* Local allocation failure: never dispatch a queued control action
         * under the network lock as an emergency fallback. */
        IHS_SessionDisconnect(channel->session);
        return;
    }
    message->type = type;
    message->header = *header;
    IHS_BufferTransferOwnership(body, &message->body);
    IHS_SessionChannelControl *control = (void *)channel;
    IHS_MutexLock(control->incomingLock);
    if (control->incomingTail)
        control->incomingTail->next = message;
    else
        control->incomingHead = message;
    control->incomingTail = message;
    ++control->incomingCount;
    IHS_MutexUnlock(control->incomingLock);
}

static IHS_IncomingControl *Pop(IHS_SessionChannelControl *control) {
    IHS_MutexLock(control->incomingLock);
    IHS_IncomingControl *message = control->incomingHead;
    if (message) {
        control->incomingHead = message->next;
        if (!control->incomingHead)
            control->incomingTail = NULL;
        --control->incomingCount;
    }
    IHS_MutexUnlock(control->incomingLock);
    return message;
}

static bool CanDispatch(int phase, EStreamControlMessage type) {
    switch (phase) {
    case IHS_SessionConnectionStateHandshaking:
        return type == k_EStreamControlServerHandshake;
    case IHS_SessionConnectionStateAuthenticating:
        return type == k_EStreamControlAuthenticationResponse;
    case IHS_SessionConnectionStateNegotiating:
        return type == k_EStreamControlNegotiationInit || type == k_EStreamControlNegotiationSetConfig;
    default:
        /* 0x7a6454 -> 0x7a6510: phases OTHER than 2/3/4 use the
         * generic 50..147 table. This is not a phase==6 test. */
        return type >= 50 && type <= 147;
    }
}

void IHS_SessionChannelControlDrainIncoming(IHS_SessionChannel *channel) {
    IHS_SessionChannelControl *control = (void *)channel;
    IHS_IncomingControl *message;
    while ((message = Pop(control))) {
        if (!atomic_load(&channel->session->transport.shutdown) &&
            CanDispatch(channel->session->state.connectionState, message->type))
            IHS_SessionChannelControlOnMessageReceived(channel, message->type, &message->body,
                                                       &message->header);
        IHS_BufferClear(&message->body, true);
        free(message);
    }
}

void IHS_SessionChannelControlClearIncoming(IHS_SessionChannel *channel) {
    IHS_SessionChannelControl *control = (void *)channel;
    if (!control->incomingLock) return;
    IHS_IncomingControl *message;
    while ((message = Pop(control))) {
        IHS_BufferClear(&message->body, true);
        free(message);
    }
}
