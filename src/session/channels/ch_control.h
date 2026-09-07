/*
 *  _____  _   _  _____  _  _  _     
 * |_   _|| | | |/  ___|| |(_)| |     Steam    
 *   | |  | |_| |\ `--. | | _ | |__     In-Home
 *   | |  |  _  | `--. \| || || '_ \      Streaming
 *  _| |_ | | | |/\__/ /| || || |_) |       Library
 *  \___/ \_| |_/\____/ |_||_||_.__/
 *
 * Copyright (c) 2022 Mariotaku <https://github.com/mariotaku>.
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 */

#pragma once

#include "channel.h"

#include "session/frame.h"
#include "session/window.h"
#include "ihs_timer.h"
#include "ihs_thread.h"

#include "protobuf/remoteplay.pb-c.h"
#include "protobuf/hiddevices.pb-c.h"

/* Official input path: reports are sent on submit (one batched message per
 * 8ms tick); reliable delivery is handled entirely by the transport
 * (retransmission until ACK/NACK). See docs/STEAMLINK_PROTOCOL_RE.md §9. */

typedef struct IHS_SessionChannelControl {
    IHS_SessionChannel base;
    /** Serializes IHS_SessionChannelControlSend: it allocates a packet id and an
     * encryption sequence, and the two must stay in step. Three threads send control
     * messages — the receive thread answering HID requests, the timer thread's
     * keepalive, and the application's per-frame HID report. */
    IHS_Mutex *sendLock;
    IHS_Mutex *receiveLock;
    IHS_TimerTask *feedbackTimer;
    uint32_t peerTimestamp, peerReceiveTime;
    bool receivedReliable;
    uint64_t sendEncryptSequence;
    uint64_t recvEncryptSequence;
    uint64_t hidSubmitted;
    uint64_t hidCoalesced;
    uint64_t hidSent;
    IHS_SessionPacketsWindow *framePacketWindow;
    /** Set once the frame window overflowed, so we disconnect only once. */
    bool overflowed;
    IHS_TimerTask *keepAliveTimer;
    /** Gap-NACK cadence: official hole-age threshold ~1ms with 5ms update
     * ticks ([f47000+2444], 0x7fe284); see docs/STEAMLINK_PROTOCOL_RE.md §9.1.
     * See protocol RE §14 for receive-watermark and echo evidence. */
    uint64_t lastNackSentMs;
    uint32_t lastNackTimestamp;
    bool haveNackTimestamp;
} IHS_SessionChannelControl;

IHS_SessionChannel *IHS_SessionChannelControlCreate(IHS_Session *session);

bool IHS_SessionChannelControlSend(IHS_SessionChannel *channel, EStreamControlMessage type,
                                   const ProtobufCMessage *message, int32_t packetId);

/** Drain SD-persisted HID report records (formatted lines) for the diag
 * writer; called from the diag disk thread only. */
size_t IHS_SessionChannelControlDrainPendingHIDReports(char *out, size_t cap);

void IHS_SessionChannelControlGetRecentHIDReports(uint64_t *out_ms, uint16_t *out_len,
                                                  uint8_t *out_data, size_t *out_off);

/** Submit one ordered CHID report batch without waiting for earlier ACKs. */
bool IHS_SessionChannelControlSubmitHIDReport(IHS_SessionChannel *channel,
                                              const uint8_t *data, size_t dataLen,
                                              bool activeInput);

/** Compatibility no-op: submissions are enqueued immediately. */
bool IHS_SessionChannelControlFlushPendingHID(IHS_SessionChannel *channel);

void IHS_SessionChannelControlHandshake(IHS_SessionChannel *channel, bool networkTest);


void IHS_SessionChannelControlRequestAuthentication(IHS_SessionChannel *channel);


void IHS_SessionChannelControlOnAuthentication(IHS_SessionChannel *channel, EStreamControlMessage type,
                                               IHS_Buffer *payload, const IHS_SessionPacketHeader *header);


void IHS_SessionChannelControlOnNegotiation(IHS_SessionChannel *channel, EStreamControlMessage type,
                                            IHS_Buffer *payload, const IHS_SessionPacketHeader *header);

void IHS_SessionChannelControlOnVideo(IHS_SessionChannel *channel, EStreamControlMessage type,
                                      IHS_Buffer *payload, const IHS_SessionPacketHeader *header);

void IHS_SessionChannelControlOnAudio(IHS_SessionChannel *channel, EStreamControlMessage type,
                                      IHS_Buffer *payload, const IHS_SessionPacketHeader *header);

void IHS_SessionChannelControlOnMicrophone(IHS_SessionChannel *channel, EStreamControlMessage type,
                                           IHS_Buffer *payload, const IHS_SessionPacketHeader *header);

void IHS_SessionChannelControlOnCursor(IHS_SessionChannel *channel, EStreamControlMessage type,
                                       IHS_Buffer *payload, const IHS_SessionPacketHeader *header);

void IHS_SessionChannelControlStartHeartbeat(IHS_SessionChannel *channel);

void IHS_SessionChannelControlStopHeartbeat(IHS_SessionChannel *channel);

void IHS_SessionChannelControlOnHIDMsg(IHS_SessionChannel *channel, const CHIDMessageToRemote *message);

bool IHS_SessionChannelControlSendHIDMsg(IHS_SessionChannel *channel, const CHIDMessageFromRemote *message);

/**
 * Top-level dispatcher for an inbound control message. Decodes the
 * EStreamControlMessage type-tagged payload and routes it to a per-feature
 * sub-handler (Video / Audio / Cursor / HID / ...) or processes it inline
 * (SetCapslock / SetKeymap / ...).
 *
 * Internal — exposed in the header so tests can drive the dispatcher
 * directly without constructing a full IHS_SessionPacket round trip.
 */
void IHS_SessionChannelControlOnMessageReceived(IHS_SessionChannel *channel, EStreamControlMessage type,
                                                IHS_Buffer *payload, const IHS_SessionPacketHeader *header);

/* IHS_Init/Quit own the diagnostic mutex; all consumers must stop before Quit. */
void IHS_ControlDiagnosticsInit(void);
void IHS_ControlDiagnosticsQuit(void);

void IHS_SessionChannelControlUpdateFeedback(IHS_SessionChannelControl *control);
