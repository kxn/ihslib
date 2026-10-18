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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "common.h"

typedef struct IHS_Client IHS_Client;

/**
 * Corresponding to ERemoteDeviceAuthorizationResult
 */
typedef enum IHS_AuthorizationResult {
    IHS_AuthorizationSuccess = 0,
    IHS_AuthorizationDenied = 1,
    IHS_AuthorizationNotLoggedIn = 2,
    IHS_AuthorizationOffline = 3,
    IHS_AuthorizationBusy = 4,
    IHS_AuthorizationInProgress = 5,
    IHS_AuthorizationTimedOut = 6,
    IHS_AuthorizationFailed = 7,
    IHS_AuthorizationCanceled = 8,
} IHS_AuthorizationResult;

typedef enum IHS_StreamingResult {
    IHS_StreamingSuccess = 0,
    IHS_StreamingUnauthorized = 1,
    IHS_StreamingScreenLocked = 2,
    IHS_StreamingFailed = 3,
    IHS_StreamingBusy = 4,
    IHS_StreamingInProgress = 5,
    IHS_StreamingCanceled = 6,
    IHS_StreamingDriversNotInstalled = 7,
    IHS_StreamingDisabled = 8,
    IHS_StreamingBroadcastingActive = 9,
    IHS_StreamingVRActive = 10,
    IHS_StreamingPINRequired = 11,
    IHS_StreamingTransportUnavailable = 12,
    IHS_StreamingInvisible = 13,
    IHS_StreamingGameLaunchFailed = 14,
    IHS_StreamingTimeout = -1,
} IHS_StreamingResult;

typedef enum IHS_StreamInterface {
    IHS_StreamInterfaceDefault = 0,
    IHS_StreamInterfaceRecentGames = 1,
    IHS_StreamInterfaceBigPicture = 2,
    IHS_StreamInterfaceDesktop = 3
} IHS_StreamInterface;

typedef struct IHS_StreamingRequest {
    char pin[16];
    struct {
        bool video;
        bool audio;
        bool input;
    } streamingEnable;
    struct {
        int32_t x;
        int32_t y;
    } maxResolution;
    int32_t audioChannelCount;
    IHS_StreamInterface streamingInterface;
    /**
     * Stream the host's whole desktop. When false the host streams its game /
     * Big Picture session instead, which is also the only path on which some
     * hosts offer HEVC.
     */
    bool streamDesktop;
    /**
     * Reserve this many gamepad slots in the streaming request, the way Steam's
     * own client does. The host allocates the slots up front instead of learning
     * of the pads only when the HID channel enumerates them mid-negotiation.
     * The controller type is left for HID detection (vid:pid), so glyphs stay
     * correct. 0 sends no reservation.
     */
    int32_t gamepadCount;
    /** Optional real game ID from host activity. Zero starts the selected interface. */
    uint64_t gameId;
    /** Request a dedicated network-test endpoint/key (discovery field 12).
     * The normal success callback supplies its credentials. Create a fresh
     * Session, configure its network-test mode, then connect; no game/decoder
     * or settings change is implied. False requests ordinary streaming. */
    bool networkTest;
    /** Selected host identity for caller-side key lookup and Proof writes. Captured
     * at creation, never serialized or replaced by AuthorizationResponse.steamid. */
    uint64_t keySteamId;
} IHS_StreamingRequest;

typedef struct IHS_ClientDiscoveryCallbacks {
    void (*discovered)(IHS_Client *client, const IHS_HostInfo *host, void *context);
} IHS_ClientDiscoveryCallbacks;

typedef struct IHS_ClientAuthorizationCallbacks {
    void (*progress)(IHS_Client *client, const IHS_HostInfo *host, void *context);

    /**
     * The host accepted the PIN. The client's secret key is now the one negotiated with this
     * host, see IHS_ClientGetSecretKey.
     */
    void (*success)(IHS_Client *client, const IHS_HostInfo *host, uint64_t steamId, void *context);

    void (*failed)(IHS_Client *client, const IHS_HostInfo *host, IHS_AuthorizationResult result,
                   void *context);

    /** Attempt host-key persistence after selecting the new secret and before
     * AuthorizationConfirmed. Failure is an application diagnostic, not a
     * different protocol result. Do not destroy/cancel/replace a request from this observer; enqueue actions. */
    void (*persist)(IHS_Client *client, const IHS_HostInfo *host, uint64_t steamId,
                    const uint8_t secretKey[32], void *context);
} IHS_ClientAuthorizationCallbacks;

typedef struct IHS_ClientStreamingCallbacks {
    void (*progress)(IHS_Client *client, const IHS_HostInfo *host, void *context);

    void (*success)(IHS_Client *client, const IHS_HostInfo *host, const IHS_SocketAddress *address,
                    const uint8_t *sessionKey, size_t sessionKeyLen, void *context);

    void (*failed)(IHS_Client *client, const IHS_HostInfo *host, IHS_StreamingResult result,
                   void *context);
    /** Same stream owner enters authorization (pin is four digits), or resumes
     * streaming/accepts credentials (pin=NULL). Pairing and connect PIN differ.
     * Callbacks may enqueue actions; do not synchronously replace/cancel/destroy
     * this owner from authorization or persist observers. */
    void (*authorization)(IHS_Client *, const IHS_HostInfo *, const char *pin,
                          uint64_t steamId, void *context);
    void (*authorizationFailed)(IHS_Client *, const IHS_HostInfo *, IHS_AuthorizationResult,
                                void *context);
    /** Proof rotation selected a new secret. Attempt storage before encryption/
     * reply, using the original request's keySteamId. Failure does not change
     * the protocol result. Enqueue actions; do not reenter/cancel/replace/destroy
     * the owner or change its secret from this observer. */
    void (*persistProof)(IHS_Client *, const IHS_HostInfo *, uint64_t keySteamId,
                         const uint8_t secret[32], void *context);
} IHS_ClientStreamingCallbacks;

IHS_Client *IHS_ClientCreate(const IHS_ClientConfig *config);

void IHS_ClientSetLogFunction(IHS_Client *client, IHS_LogFunction *logFunction);

/**
 * Get the current 32 byte secret key.
 *
 * Steam pairs devices with a key exchange, so the secret key is specific to each host. After a
 * successful authorization the client switches to the key negotiated with that host: store it,
 * and set it again (here or with IHS_ClientConfig.secretKey) before requesting streaming from
 * the same host.
 */
void IHS_ClientGetSecretKey(IHS_Client *client, uint8_t *secretKey);

/**
 * Switch to another 32 byte secret key, e.g. the one stored for the host you're about to stream
 * from.
 */
void IHS_ClientSetSecretKey(IHS_Client *client, const uint8_t *secretKey);

void IHS_ClientStop(IHS_Client *client);

void IHS_ClientThreadedJoin(IHS_Client *client);

void IHS_ClientDestroy(IHS_Client *client);

void IHS_ClientSetDiscoveryCallbacks(IHS_Client *client,
                                     const IHS_ClientDiscoveryCallbacks *callbacks, void *context);

void IHS_ClientSetAuthorizationCallbacks(IHS_Client *client,
                                         const IHS_ClientAuthorizationCallbacks *callbacks,
                                         void *context);

/** Configure before starting requests. Deferred delivery serializes authorization
 * and streaming handshake callbacks with an application's state/storage actor. The RX worker queues
 * bounded owned messages; Pump releases them after dispatch. Keep the client
 * alive through each Pump, and stop/join RX before destroying it. */
void IHS_ClientSetAuthorizationDeferred(IHS_Client *client, bool deferred);
size_t IHS_ClientPumpAuthorization(IHS_Client *client, size_t budget);

void IHS_ClientSetStreamingCallbacks(IHS_Client *client,
                                     const IHS_ClientStreamingCallbacks *callbacks, void *context);

const char *IHS_ClientError(IHS_Client *client);

/* ----------------------------------------------------
 * - Discovery functions
 * ---------------------------------------------------- */

/**
 * Start discovery
 * @param client Client instance
 * @param interval Milliseconds between each discovery request. 0 for single-shot
 * @return
 */
bool IHS_ClientStartDiscovery(IHS_Client *client, uint32_t interval);

bool IHS_ClientStopDiscovery(IHS_Client *client);

/* ----------------------------------------------------
 * - Authorization functions
 * ---------------------------------------------------- */

/**
 * Request authorization
 * @param client Client instance
 * @param host Host information
 * @param pin PIN code
 */
bool IHS_ClientAuthorizationRequest(IHS_Client *client, const IHS_HostInfo *host, const char *pin);

bool IHS_ClientAuthorizationCancel(IHS_Client *client);

/* ----------------------------------------------------
 * - Streaming functions
 * ---------------------------------------------------- */

/** Request ordinary streaming or a network-test ticket. Waiting/Busy stream
 * sends use time()+1; the same owner's authorization sends use time()+3 until
 * a terminal response or explicit StreamingCancel. This
 * API does not invent a silent-host deadline. Unauthorized enters same-owner
 * authorization; callbacks report the pairing code and resume. Credentials are borrowed during success.
 * A repeated request to the same host while waiting is ignored. A new request
 * replaces ticket ownership without sending Cancel; explicit Cancel is separate. */
bool IHS_ClientStreamingRequest(IHS_Client *client, const IHS_HostInfo *host,
                                const IHS_StreamingRequest *request);

/** Cancel the current request, including one that already supplied a ticket,
 * preserving a bounded late-response/host record. */
bool IHS_ClientStreamingCancel(IHS_Client *client);

/** Stop waiting after consuming the ticket. Keep the ID available for explicit
 * Cancel; replacement Request retires it without cancellation (RE §124).
 * Handshake ownership remains alive for late authorization/Proof messages. */
void IHS_ClientStreamingEstablished(IHS_Client *client);
