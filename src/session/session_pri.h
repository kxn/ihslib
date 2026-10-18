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

#include "base.h"
#include "clock.h"
#include "ihslib/session.h"
#include "packet.h"
#include "outgoing.h"
#include "reliable.h"
#include "retransmission.h"
#include <stdatomic.h>

#include "channels/channel.h"

#include "ihs_queue.h"
#include "protobuf/remoteplay.pb-c.h"

/**
 * Connection phase. Advances monotonically from Unconnected to Connected; never goes
 * backwards within a single session lifetime (a re-connect requires a fresh session).
 * Guards on the message-handling entry points use this to drop stale or replayed
 * packets that would otherwise re-trigger an earlier phase.
 *
 *   Unconnected   - IHS_SessionCreate just returned, no Connect packet sent yet.
 *   Connecting    - Connect packet sent on the discovery channel, waiting for ACK.
 *   Handshaking   - ConnectACK received, ClientHandshake sent, waiting for ServerHandshake.
 *   Authenticating- ServerHandshake received, AuthenticationRequest sent, waiting for response.
 *   Negotiating   - AuthenticationResponse = Succeeded, waiting for NegotiationInit / sending
 * SetConfig.
 *   Starting      - Host SetConfig copied; next RX update starts the stream.
 *   Connected     - NegotiationComplete queued, the stream is live.
 *   Stopping      - Transport delegate retired; drain then stop consumers.
 */
typedef enum IHS_SessionConnectionState {
    IHS_SessionConnectionStateUnconnected,
    IHS_SessionConnectionStateConnecting,
    IHS_SessionConnectionStateHandshaking,
    IHS_SessionConnectionStateAuthenticating,
    IHS_SessionConnectionStateNegotiating,
    IHS_SessionConnectionStateStarting,
    IHS_SessionConnectionStateConnected,
    IHS_SessionConnectionStateStopping,
} IHS_SessionConnectionState;

typedef enum IHS_TransportState {
    IHS_TransportConnecting = 0,
    IHS_TransportConnected = 1,
    IHS_TransportClosing = 2,
    IHS_TransportClosed = 3,
} IHS_TransportState;

typedef struct IHS_SessionState {
    atomic_int connectionState;
    atomic_int mtu;
    atomic_uchar connectionId;
    atomic_uchar hostConnectionId;
    atomic_bool crcRequired;
    /**
     * Effective stream-enable values from the complete client config, including
     * its true defaults. Updated for initial config and incoming/local replacement;
     * receiving 99 is not an obligatory acknowledgement of a prior request.
     */
    atomic_bool streamingInput;
    atomic_bool streamingAudio;
    atomic_bool streamingVideo;
    /* Host notification for UI/diagnostics; not an outbound HID gate. */
    atomic_bool inputTemporarilyDisabled;
} IHS_SessionState;

struct IHS_FrameStatsAggregator;

struct IHS_Session {
    IHS_Base base;
    IHS_SessionInfo info;
    IHS_SessionState state;
    CStreamingClientConfig *clientConfig; /* Base lock; owned complete value. */
    CNegotiatedConfig *negotiatedConfig; /* RX-owned complete value, including unknown fields. */
    /* RX-worker-owned connection clock and lifecycle state. The application
     * publishes foreground transitions through lifecycleGeneration only. */
    struct {
        uint32_t updateTime, lastReceive, lastConnect, appliedGeneration;
        atomic_uint lifecycleGeneration;
        atomic_uint shutdown; /* 0: live phase, 1: closing, 2: closed */
        atomic_bool detached, notified, addressCleared;
        atomic_bool closeRequested; /* Public cancellation; RX owner closes after consumer stop. */
        bool active, clientPaused, connectionPaused, timedOut;
    } transport;
    /* Negotiated codec preference and capture size; StartVideoData supplies
     * the actual decoder configuration. */
    int negotiatedVideoCodec;
    /* OnNegotiationInit's true-only cached request flags (protocol §48). */
    bool negotiationRemoteHid, negotiationTouchInput;
    int32_t hostOsType, hostGamingDeviceType;
    uint32_t captureWidth, captureHeight;
    atomic_bool destroying;
    atomic_bool hostVideoStopped;
    atomic_bool hostRequestedStop; /* Explicit inbound StopRequest / Disconnect only. */
    uint8_t numChannels;
    IHS_SessionChannel *channels[16];
    IHS_Thread *sendThread;
    IHS_Cond *sendQueueCond;
    IHS_Mutex *sendQueueMutex;
    IHS_Queue *sendQueue;
    IHS_Queue *sendBatch;   /* producer-only staging, guarded by sendQueueMutex */
    IHS_Queue *pendingData; /* RX-thread-owned packets awaiting Start*Data metadata */
    unsigned pendingDataCount;
    IHS_Timer *timers;
    IHS_SessionRetransmission retransmission;
    IHS_Outgoing outgoing;
    IHS_ReliableChannel reliable[32];
    struct IHS_UnreliableChannel *unreliable[32];
    IHS_Mutex *reliableLock;
    IHS_StreamClock clock;
    /* Connection collectors, serialized by base.lock; no independent timer. */
    IHS_NetworkBitrateStats networkRates[2]; /* outgoing, incoming */
    IHS_NetworkPacketStats networkPackets;
    IHS_NetworkVideoStats networkVideo;
    IHS_HIDManager *hidManager;
    struct IHS_FrameStatsAggregator *frameStats;
    struct IHS_FrameTracker *frameTracker;
    uint64_t videoTrackingId, nextVideoEpoch;
    bool invalidVideoCallbacks;
    IHS_StreamClientCapabilities clientCapabilities; /* session-owned strings */
    char *videoDecoderInfo; /* Current backend; session-owned, Base-locked. */
    struct {
        const IHS_StreamSessionCallbacks *session;
        const IHS_StreamAudioCallbacks *audio;
        const IHS_StreamVideoCallbacks *video;
        const IHS_StreamInputCallbacks *input;
        const IHS_StreamMicrophoneCallbacks *microphone;
    } callbacks;
    struct {
        void *session;
        void *audio;
        void *video;
        void *input;
        void *microphone;
    } callbackContexts;
};

#define IHS_SessionLog(session, level, tag, ...)                                                   \
    IHS_BaseLog((IHS_Base *)(session), (level), (tag), __VA_ARGS__)

void IHS_SessionInterrupt(IHS_Session *session);

/* Worker steps, also used by the protocol oracle with a controlled clock. */
void IHS_SessionTransportBegin(IHS_Session *session, uint32_t now);
bool IHS_SessionTransportSendConnect(IHS_Session *session);
IHS_TransportState IHS_SessionTransportState(const IHS_Session *session);
uint8_t IHS_SessionNextConnectionId(void);
void IHS_SessionTransportDisconnect(IHS_Session *session);
void IHS_SessionTransportClose(IHS_Session *session);
bool IHS_SessionTransportFinish(IHS_Session *session);
void IHS_SessionNotifyDisconnected(IHS_Session *session);
/* Connection delegate and client stage are distinct from the app callback. */
void IHS_SessionClientTransportClosed(IHS_Session *session);
void IHS_SessionClientStopStreaming(IHS_Session *session);
CStreamingClientConfig *IHS_SessionClientConfigCopy(IHS_Session *session);
bool IHS_SessionClientConfigDisableHardware(IHS_Session *session);
bool IHS_SessionClientConfigStore(IHS_Session *session, const CStreamingClientConfig *config);
bool IHS_SessionClientConfigApply(IHS_Session *session, const CStreamingClientConfig *config,
                                  bool localOnly);
void IHS_SessionTransportPrepareUpdate(IHS_Session *session, uint32_t now);
void IHS_SessionTransportCheckTimeout(IHS_Session *session, uint32_t now);
/* Component prefix + deadline, without intervening channel work. */
void IHS_SessionTransportUpdate(IHS_Session *session, uint32_t now);

bool IHS_SessionSendPacket(IHS_Session *session, IHS_SessionPacket *packet);

/**
 * Add packet to send queue
 * @param session Session instance
 * @param packet Packet
 * @param retransmit Retain until cumulative/selective peer feedback confirms receipt.
 * @return Producer admission only; actual socket I/O happens on the sender worker.
 */
bool IHS_SessionQueuePacket(IHS_Session *session, IHS_SessionPacket *packet, bool retransmit);
IHS_SendAdmission IHS_SessionQueuePacketResult(IHS_Session *session, IHS_SessionPacket *packet,
                                               bool retransmit);
/* Caller holds sendQueueMutex across begin, every fragment and end. */
bool IHS_SessionSendBatchBegin(IHS_Session *session);
void IHS_SessionSendBatchEnd(IHS_Session *session, bool commit);
/* Dispatch one complete transaction; wait=true is the bounded worker wait. */
bool IHS_SessionDispatchQueued(IHS_Session *session, bool wait);
/* Batch-end per-channel RX/feedback/TX, then rotating retries; before client stage. */
void IHS_SessionNetworkUpdateAt(IHS_Session *, uint32_t tick);

bool IHS_SessionSendControlMessage(IHS_Session *session, EStreamControlMessage type,
                                   const ProtobufCMessage *message);

/**
 * @return true if the session-state input flag is set. Outbound input/HID send sites should
 *   early-return false when this is false. Mirrors Steam's BStreamingInput().
 */
bool IHS_SessionInputEnabled(IHS_Session *session);

/**
 * Mirrors Steam's IsStreaming(): live during Starting before Complete is queued.
 * Outbound control traffic that the host only expects from a live session must
 * check this — it accepts an early packet, then abandons the session.
 */
bool IHS_SessionStreaming(IHS_Session *session);

void IHS_SessionHostStopped(IHS_Session *session);

void IHS_SessionDrainPendingData(IHS_Session *session, IHS_SessionChannel *channel);
