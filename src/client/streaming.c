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

#include "client_pri.h"
#include "crypto.h"
#include "authorization_flow.h"
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>

typedef struct IHS_StreamingState {
    IHS_Client *client;
    IHS_HostInfo host;
    IHS_StreamingRequest request;
    uint32_t requestId;
    IHS_AuthorizationFlow authorization;
    bool authorizing, ticket, proofUpdated;
    int64_t nextSendWallSecond;
    uint64_t receiveGeneration;
    bool hasSent;
    bool terminal;
    bool timerFailed;
    uint8_t encryptedPin[3 * IHS_CRYPTO_AES_BLOCK_SIZE];
    size_t encryptedPinLen;
} IHS_StreamingState;

static uint64_t StreamingRequestTimer(int runCount, void *context);

static void StreamingRequestCleanup(void *context);
static bool SendStreamingRequest(IHS_StreamingState *state);
static bool SendCurrentRequest(IHS_StreamingState *state);

static void SetAuthorizing(IHS_StreamingState *state, bool value) {
    state->authorizing = value;
    IHS_BaseLock(&state->client->base);
    state->client->lastLaunchAuthorizing = value;
    IHS_BaseUnlock(&state->client->base);
}
static void ClearLaunch(IHS_StreamingState *state) {
    IHS_Client *client = state->client;
    IHS_BaseLock(&client->base);
    if (client->lastLaunchId == state->requestId) {
        client->lastLaunchId = 0;
        client->lastLaunchHasTicket = client->lastLaunchAuthorizing = false;
    }
    IHS_BaseUnlock(&client->base);
}
static void NotifyAuthorization(IHS_StreamingState *state, const char *pin, uint64_t steamId) {
    IHS_Client *client = state->client;
    if (client->callbacks.streaming && client->callbacks.streaming->authorization)
        client->callbacks.streaming->authorization(client, &state->host, pin, steamId,
                                                    client->callbackContexts.streaming);
}
static void NotifyAuthorizationFailure(IHS_Client *client, const IHS_HostInfo *host,
                                        IHS_AuthorizationResult result) {
    if (client->callbacks.streaming && client->callbacks.streaming->authorizationFailed)
        client->callbacks.streaming->authorizationFailed(client, host, result,
                                                          client->callbackContexts.streaming);
    else if (client->callbacks.streaming && client->callbacks.streaming->failed)
        client->callbacks.streaming->failed(client, host, IHS_StreamingFailed,
                                             client->callbackContexts.streaming);
}

static void SendCancel(IHS_Client *client, const IHS_HostInfo *host, uint32_t id) {
    CMsgRemoteDeviceStreamingCancelRequest msg = CMSG_REMOTE_DEVICE_STREAMING_CANCEL_REQUEST__INIT;
    msg.request_id = id;
    IHS_ClientSendControl(client, host, NULL, k_ERemoteDeviceStreamingCancelRequest,
                          (ProtobufCMessage *)&msg);
}
static void CancelVisit(IHS_TimerTask *task, void *context) {
    IHS_UNUSED(task);
    IHS_Client *client = context;
    IHS_TimerTaskStopOwned(client->timers, &client->taskHandles.streaming);
}
/* Stop waiting before changing ownership, but send no cancellation: original
 * StartStreaming replaces state5(other host)/state6 directly (RE §124). */
static void RetireForReplacement(IHS_Client *client) {
    IHS_TimerTaskVisitOwned(client->timers, &client->taskHandles.streaming, CancelVisit, client);
    IHS_BaseLock(&client->base);
    if (client->lastLaunchId) {
        unsigned i = client->canceledNext++ % 8;
        client->canceled[i].host = client->lastLaunchHost;
        client->canceled[i].requestId = client->lastLaunchId;
    }
    client->lastLaunchId = 0;
    client->lastLaunchHasTicket = client->lastLaunchAuthorizing = false;
    IHS_BaseUnlock(&client->base);
}
bool IHS_ClientStreamingCancel(IHS_Client *client) {
    /* Visit locks out callbacks/retries before retiring the transaction. */
    IHS_TimerTaskVisitOwned(client->timers, &client->taskHandles.streaming, CancelVisit, client);
    IHS_BaseLock(&client->base);
    uint32_t id = client->lastLaunchId;
    bool authorizing = client->lastLaunchAuthorizing;
    IHS_HostInfo host = client->lastLaunchHost;
    if (id) {
        unsigned i = client->canceledNext++ % 8;
        client->canceled[i].host = host;
        client->canceled[i].requestId = id;
        client->lastLaunchId = 0;
    }
    client->lastLaunchHasTicket = client->lastLaunchAuthorizing = false;
    IHS_BaseUnlock(&client->base);
    if (id) {
        if (authorizing) IHS_ClientAuthorizationSendCancel(client, &host);
        else SendCancel(client, &host, id);
    }
    return id != 0;
}

bool IHS_ClientStreamingRequest(IHS_Client *client, const IHS_HostInfo *host,
                                const IHS_StreamingRequest *request) {
    IHS_BaseLock(&client->base);
    bool sameWaitingHost = client->lastLaunchId && !client->lastLaunchHasTicket &&
                           !client->lastLaunchAuthorizing &&
                           client->lastLaunchHost.clientId == host->clientId;
    IHS_BaseUnlock(&client->base);
    if (sameWaitingHost)
        return true;
    IHS_ClientLog(client, IHS_LogLevelInfo, "Client", "Begin sending streaming request to host %s",
                  host->hostname);
    IHS_StreamingState *state = calloc(1, sizeof(IHS_StreamingState));
    if (!state)
        return false;
    state->terminal = state->timerFailed = false;
    state->client = client;
    state->host = *host;
    state->request = *request;
    RetireForReplacement(client);
    bool collision;
    do {
        state->requestId = IHS_CryptoRandomUInt32();
        collision = !state->requestId;
        IHS_BaseLock(&client->base);
        for (unsigned i = 0; i < 8; ++i)
            collision |= client->canceled[i].requestId == state->requestId;
        IHS_BaseUnlock(&client->base);
    } while (collision);
    IHS_BaseLock(&client->base);
    client->lastLaunchId = state->requestId;
    client->lastLaunchHost = *host;
    client->lastLaunchHasTicket = client->lastLaunchAuthorizing = false;
    state->receiveGeneration = client->receiveGeneration;
    IHS_BaseUnlock(&client->base);
    uint32_t random;
    do {
        if (IHS_CryptoRandomBytes((uint8_t *)&random, sizeof(random)) != 0) {
            ClearLaunch(state);
            free(state);
            return false;
        }
    } while (random % 10000 == 0);
    char pairingPin[5];
    snprintf(pairingPin, sizeof(pairingPin), "%04u", (unsigned)(random % 10000));
    if (!IHS_ClientAuthorizationFlowInit(&state->authorization, pairingPin)) {
        ClearLaunch(state);
        free(state);
        return false;
    }
    /* Official BSetStreamingPIN encrypts once when creating the transaction.
     * Retries and Unauthorized resumption reuse that blob (protocol RE §129). */
    state->encryptedPinLen = 0;
    size_t pinLen = strnlen(request->pin, sizeof(request->pin));
    if (pinLen) {
        uint8_t secret[32];
        IHS_BaseGetSecretKey(&client->base, secret);
        state->encryptedPinLen = sizeof(state->encryptedPin);
        int result = IHS_CryptoSymmetricEncrypt((const uint8_t *)request->pin, pinLen,
            secret, sizeof(secret), state->encryptedPin, &state->encryptedPinLen);
        memset(secret, 0, sizeof(secret));
        if (result != 0) {
            IHS_ClientStreamingCancel(client);
            IHS_AuthorizationKeyExchangeClear(&state->authorization.keyExchange);
            free(state);
            return false;
        }
    }
    if (!IHS_TimerTaskStartOwned(client->timers, &client->taskHandles.streaming,
                                 StreamingRequestTimer, StreamingRequestCleanup, 25, state)) {
        IHS_BaseLock(&client->base);
        if (client->lastLaunchId == state->requestId) {
            client->lastLaunchId = 0;
            client->lastLaunchHasTicket = client->lastLaunchAuthorizing = false;
        }
        IHS_BaseUnlock(&client->base);
        IHS_AuthorizationKeyExchangeClear(&state->authorization.keyExchange);
        free(state);
        return false;
    }
    return true;
}

typedef struct {
    IHS_Client *client;
    CMsgRemoteClientBroadcastHeader *header;
    ProtobufCMessage *message;
    const IHS_SocketAddress *address;
} StreamingResponseContext;

static void StreamingResponseVisit(IHS_TimerTask *timer, void *context) {
    StreamingResponseContext *responseContext = context;
    IHS_Client *client = responseContext->client;
    CMsgRemoteClientBroadcastHeader *header = responseContext->header;
    ProtobufCMessage *message = responseContext->message;
    IHS_StreamingState *state = IHS_TimerTaskGetContext(timer);
    if (state->terminal)
        return;
    if (client->callbacks.routing) {
        /* BProcess already requires nonzero identity. Original response and
         * progress handlers match request_id, not source IP/instance/target ID. */
        if (!header->client_id)
            return;
    } else if (!IHS_ClientMatchesHost(client, &state->host, responseContext->address, header))
        return;
    if (state->ticket && header->msg_type != k_ERemoteDeviceProofRequest) return;
    uint32_t id;
    switch (header->msg_type) {
    case k_ERemoteDeviceStreamingResponse: id = ((CMsgRemoteDeviceStreamingResponse *)message)->request_id; break;
    case k_ERemoteDeviceStreamingProgress: id = ((CMsgRemoteDeviceStreamingProgress *)message)->request_id; break;
    default: return;
    }
    if (id != state->requestId) return;
    switch (header->msg_type) {
    case k_ERemoteDeviceStreamingProgress:
        if (client->callbacks.streaming && client->callbacks.streaming->progress)
            client->callbacks.streaming->progress(client, &state->host, client->callbackContexts.streaming);
        break;
    case k_ERemoteDeviceStreamingResponse: {
        CMsgRemoteDeviceStreamingResponse *response = (CMsgRemoteDeviceStreamingResponse *)message;
        if (response->request_id != state->requestId)
            return;
        switch (response->result) {
        case k_ERemoteDeviceStreamingBusy:
        case k_ERemoteDeviceStreamingInProgress:
            IHS_ClientLog(client, IHS_LogLevelDebug, "Client",
                          "Streaming request in progress: host %s", state->host.hostname);
            if (client->callbacks.streaming && client->callbacks.streaming->progress) {
                client->callbacks.streaming->progress(client, &state->host,
                                                      client->callbackContexts.streaming);
            }
            return;
        case k_ERemoteDeviceStreamingUnauthorized: {
            bool entering = !state->authorizing;
            SetAuthorizing(state, true);
            if (!SendCurrentRequest(state)) {
                IHS_TimerTaskStop(timer);
                return;
            }
            if (entering) NotifyAuthorization(state, state->authorization.pin, 0);
            return;
        }
        case k_ERemoteDeviceStreamingSuccess: {
            ProtobufCBinaryData enc = response->encrypted_session_key;
            uint8_t key[128];
            size_t keyLen = sizeof(key);
            uint8_t secret[32];
            IHS_BaseGetSecretKey(&client->base, secret);
            int decryptRet = IHS_CryptoSymmetricDecrypt(
                enc.data, enc.len, secret, sizeof(secret), key, &keyLen);
            memset(secret, 0, sizeof(secret));
            if (decryptRet != 0) {
                state->terminal = true;
                IHS_TimerTaskStop(timer);
                /* Invalid credentials stop the current protocol phase. */
                if (state->authorizing) IHS_ClientAuthorizationSendCancel(client, &state->host);
                else SendCancel(client, &state->host, state->requestId);
                ClearLaunch(state);
                if (client->callbacks.streaming && client->callbacks.streaming->failed)
                    client->callbacks.streaming->failed(client, &state->host, IHS_StreamingFailed,
                                                         client->callbackContexts.streaming);
                return;
            }
            bool wasAuthorizing = state->authorizing;
            SetAuthorizing(state, false);
            state->ticket = true;
            IHS_BaseLock(&client->base);
            client->lastLaunchHasTicket = true;
            IHS_BaseUnlock(&client->base);
            if (wasAuthorizing) NotifyAuthorization(state, NULL, 0);
            /* Authorization notification may enqueue work but must not replace
             * this owner synchronously; see the callback contract. */
            if (client->callbacks.streaming && client->callbacks.streaming->connection) {
                IHS_StreamingConnection connection = {.clientId = header->client_id,
                                                      .transport = response->transport,
                                                      .port = response->port,
                                                      .address = *responseContext->address,
                                                      .sessionKey = key,
                                                      .sessionKeyLen = keyLen,
                                                      .certificate = response->cert,
                                                      .relayServer = response->relay_server};
                if (IHS_ClientAddressHasIP(&connection.address.ip) && connection.address.port)
                    connection.address.port = (uint16_t)response->port;
                client->callbacks.streaming->connection(client, &state->host, &connection,
                                                        client->callbackContexts.streaming);
            } else if (client->callbacks.streaming && client->callbacks.streaming->success) {
                IHS_SocketAddress streamingAddress = {responseContext->address->ip, response->port};
                client->callbacks.streaming->success(client, &state->host, &streamingAddress,
                                                     key, keyLen, client->callbackContexts.streaming);
            }
            return;
        }
        default:
            state->terminal = true;
            IHS_TimerTaskStop(timer);
            if (state->authorizing) IHS_ClientAuthorizationSendCancel(client, &state->host);
            else SendCancel(client, &state->host, state->requestId);
            ClearLaunch(state);
            if (client->callbacks.streaming && client->callbacks.streaming->failed)
                client->callbacks.streaming->failed(client, &state->host,
                    (IHS_StreamingResult)response->result, client->callbackContexts.streaming);
            return;
        }
        break;
    }
    default:
        break;
    }
}

static void SendEmptyProof(IHS_Client *client, IHS_SocketAddress address, uint64_t clientId) {
    CMsgRemoteDeviceProofResponse response = CMSG_REMOTE_DEVICE_PROOF_RESPONSE__INIT;
    IHS_HostInfo peer = {.clientId = clientId};
    IHS_ClientSendControl(client, &peer, &address, k_ERemoteDeviceProofResponse, &response.base);
}
static void ProofVisit(IHS_TimerTask *task, void *context) {
    StreamingResponseContext *incoming = context;
    IHS_Client *client = incoming->client;
    IHS_StreamingState *state = IHS_TimerTaskGetContext(task);
    const CMsgRemoteDeviceProofRequest *request = (void *)incoming->message;
    /* Original handler tests client identity before optional request_id. Port/
     * instance are not a substitute for the current target (RE §133). */
    if (state->terminal || !incoming->header->client_id ||
        incoming->header->client_id != state->host.clientId) {
        SendEmptyProof(client, *incoming->address, incoming->header->client_id);
        return;
    }
    if (request->has_request_id && request->request_id != state->requestId)
        return;
    uint8_t secret[32];
    IHS_BaseGetSecretKey(&client->base, secret);
    if (request->update_secret && !state->proofUpdated) {
        if (request->challenge.len > SIZE_MAX - sizeof(secret)) {
            memset(secret, 0, sizeof(secret));
            return;
        }
        size_t count = sizeof(secret) + request->challenge.len;
        uint8_t *input = malloc(count);
        if (!input) {
            memset(secret, 0, sizeof(secret));
            return;
        } /* Local allocation failure, not a Source OOM claim. */
        memcpy(input, secret, sizeof(secret));
        if (request->challenge.len)
            memcpy(input + sizeof(secret), request->challenge.data, request->challenge.len);
        int hashed = IHS_CryptoSHA256(input, count, secret);
        memset(input, 0, sizeof(secret));
        free(input);
        if (hashed != 0) {
            memset(secret, 0, sizeof(secret));
            return;
        }
        IHS_BaseSetSecretKey(&client->base, secret);
        if (client->callbacks.streaming && client->callbacks.streaming->persistProof)
            client->callbacks.streaming->persistProof(client, &state->host,
                                                      state->request.keySteamId, secret,
                                                      client->callbackContexts.streaming);
        state->proofUpdated = true;
    }
    /* Original CUtlBuffer allocates from challenge length. Do not inherit the
     * former fixed 1024-byte output limit for valid discovery packets. */
    if (request->challenge.len > SIZE_MAX - 2 * IHS_CRYPTO_AES_BLOCK_SIZE) {
        memset(secret, 0, sizeof(secret));
        return;
    }
    size_t capacity =
        (request->challenge.len / IHS_CRYPTO_AES_BLOCK_SIZE + 2) * IHS_CRYPTO_AES_BLOCK_SIZE;
    uint8_t *encrypted = malloc(capacity);
    if (!encrypted) {
        memset(secret, 0, sizeof(secret));
        return;
    }
    size_t length = capacity;
    int result = IHS_CryptoSymmetricEncrypt(request->challenge.data, request->challenge.len, secret,
                                            sizeof(secret), encrypted, &length);
    memset(secret, 0, sizeof(secret));
    if (result == 0 && length <= capacity) {
        CMsgRemoteDeviceProofResponse response = CMSG_REMOTE_DEVICE_PROOF_RESPONSE__INIT;
        response.has_request_id = response.has_updated_secret = true;
        response.request_id = state->requestId;
        response.updated_secret = request->update_secret;
        response.response = (ProtobufCBinaryData){length, encrypted};
        IHS_ClientSendControl(client, &state->host, incoming->address, k_ERemoteDeviceProofResponse,
                              (ProtobufCMessage *)&response);
    }
    free(encrypted);
}

void IHS_ClientStreamingCallback(IHS_Client *client, const IHS_SocketAddress *address,
                                 CMsgRemoteClientBroadcastHeader *header,
                                 ProtobufCMessage *message) {
    if (!message)
        return; /* Local guard. Official BProcess has parse-error fallbacks;
                 * envelope/admission equivalence remains separate (RE §130). */
    if (header->msg_type == k_ERemoteDeviceProofRequest) {
        StreamingResponseContext context = {client, header, message, address};
        if (!IHS_TimerTaskVisitOwned(client->timers, &client->taskHandles.streaming, ProofVisit, &context))
            SendEmptyProof(client, *address, header->client_id);
        return;
    }
    if (header->msg_type == k_ERemoteDeviceStreamingResponse) {
        CMsgRemoteDeviceStreamingResponse *resp = (void *)message;
        IHS_HostInfo knownHost;
        bool known = false, stale;
        IHS_BaseLock(&client->base);
        stale = !resp->request_id || resp->request_id != client->lastLaunchId;
        if (IHS_ClientMatchesHost(client, &client->lastLaunchHost, address, header)) {
            knownHost = client->lastLaunchHost;
            known = true;
        }
        for (unsigned i = 0; !known && i < 8; ++i) {
            if (client->canceled[i].requestId &&
                IHS_ClientMatchesHost(client, &client->canceled[i].host, address, header)) {
                knownHost = client->canceled[i].host;
                known = true;
            }
        }
        IHS_BaseUnlock(&client->base);
        if (stale) {
            if (client->callbacks.routing && resp->result == k_ERemoteDeviceStreamingInProgress) {
                /* This inline original cancellation is an explicit reply to
                 * the packet source, unlike StopStreaming's implicit cancel. */
                IHS_HostInfo peer = {.clientId = header->client_id};
                CMsgRemoteDeviceStreamingCancelRequest cancel =
                    CMSG_REMOTE_DEVICE_STREAMING_CANCEL_REQUEST__INIT;
                cancel.request_id = resp->request_id;
                IHS_ClientSendControl(client, &peer, address, k_ERemoteDeviceStreamingCancelRequest,
                                      &cancel.base);
                return;
            }
            /* Official handler cancels every mismatched InProgress, including
             * zero/unknown IDs, without a time or cancelled-ID-cache guard.
             * Only a previously selected, verified host can reach this send. */
            if (known && resp->result == k_ERemoteDeviceStreamingInProgress)
                SendCancel(client, &knownHost, resp->request_id);
            return;
        }
    }
    StreamingResponseContext context = {client, header, message, address};
    IHS_TimerTaskVisitOwned(client->timers, &client->taskHandles.streaming, StreamingResponseVisit,
                            &context);
}

static bool SendStreamingRequest(IHS_StreamingState *state) {
    IHS_Client *client = state->client;
    IHS_HostInfo host = state->host;
    if (client->callbacks.routing && client->callbacks.routing->host &&
        !client->callbacks.routing->host(client, host.clientId, &host,
                                         client->callbackContexts.routing))
        return false;
    IHS_StreamingRequest request = state->request;

    CMsgRemoteDeviceStreamingRequest message = CMSG_REMOTE_DEVICE_STREAMING_REQUEST__INIT;
    message.request_id = state->requestId;

    message.has_client_id = true;
    message.client_id = host.clientId;

    message.has_pin = true;
    message.pin.len = state->encryptedPinLen;
    message.pin.data = state->encryptedPin;

    message.has_maximum_resolution_x = request.maxResolution.x > 0;
    message.maximum_resolution_x = request.maxResolution.x;
    message.has_maximum_resolution_y = request.maxResolution.y > 0;
    message.maximum_resolution_y = request.maxResolution.y;

    message.has_enable_audio_streaming = true;
    message.enable_audio_streaming = request.streamingEnable.audio;

    message.has_enable_video_streaming = true;
    message.enable_video_streaming = request.streamingEnable.video;

    message.has_enable_input_streaming = true;
    message.enable_input_streaming = request.streamingEnable.input;

    message.has_audio_channel_count = true;
    message.audio_channel_count = request.audioChannelCount;

    message.has_restricted = true;
    message.restricted = false;

    message.has_stream_interface = true;
    message.stream_interface = (EStreamInterface)request.streamingInterface;

    message.has_stream_desktop = true;
    message.stream_desktop = request.streamDesktop;
    message.has_network_test = true;
    message.network_test = request.networkTest;
    if (request.gameId != 0) {
        message.has_gameid = true;
        message.gameid = request.gameId;
    }

    message.has_form_factor = true;
    message.form_factor = k_EStreamDeviceFormFactorTV;

    /* Without a device_version the host ignores maximum_resolution_x/y and streams
     * at its own capture size. It does NOT gate the advertised codec list on this,
     * nor on anything else in this message: CMsgRemoteDeviceStreamingRequest has no
     * codec field at all. The host offers what its encoder can produce. */
    message.device_version = request.allowSDR ? "1.3.32" : "1.1.0";

    uint8_t secret[32], token[32];
    IHS_BaseGetSecretKey(&client->base, secret);
    bool tokenReady = IHS_ClientCreateDeviceToken(client, secret, token);
    memset(secret, 0, sizeof(secret));
    if (!tokenReady) return false;
    message.has_device_token = true;
    message.device_token.data = token;
    message.device_token.len = sizeof(token);

    message.n_supported_transport = request.allowSDR ? 2 : 1;
    EStreamTransport transports[] = {k_EStreamTransportUDP, k_EStreamTransportSDR};
    message.supported_transport = transports;

    /* Reserve gamepad slots the way Steam does: the host sets them up from the
     * request rather than waiting for the HID channel to enumerate mid-stream.
     * controller_type is left unset so the host still types each pad from its
     * HID vid:pid. Steam sends both the count and one entry per slot. */
    CMsgRemoteDeviceStreamingRequest__ReservedGamepad reserved[16];
    CMsgRemoteDeviceStreamingRequest__ReservedGamepad *reservedPtrs[16];
    int gamepadCount = request.gamepadCount;
    if (gamepadCount > 16)
        gamepadCount = 16;
    if (gamepadCount > 0) {
        message.has_gamepad_count = true;
        message.gamepad_count = gamepadCount;
        for (int i = 0; i < gamepadCount; i++) {
            cmsg_remote_device_streaming_request__reserved_gamepad__init(&reserved[i]);
            reservedPtrs[i] = &reserved[i];
        }
        message.n_gamepads = gamepadCount;
        message.gamepads = reservedPtrs;
    }

    IHS_ClientLog(client, IHS_LogLevelDebug, "Client",
                  "Sending streaming request packet to host %s", host.hostname);
    return IHS_ClientSendControl(client, &host, NULL, k_ERemoteDeviceStreamingRequest,
                                 &message.base);
}

static void StreamingRequestCleanup(void *context) {
    IHS_StreamingState *state = context;
    IHS_AuthorizationKeyExchangeClear(&state->authorization.keyExchange);
    if (state->timerFailed && state->client->callbacks.streaming &&
        state->client->callbacks.streaming->failed)
        state->client->callbacks.streaming->failed(state->client, &state->host,
            IHS_StreamingFailed, state->client->callbackContexts.streaming);
    free(context);
}

void IHS_ClientStreamingEstablished(IHS_Client *client) {
    IHS_BaseLock(&client->base);
    /* The successful manager state keeps its ID until explicit Stop or a
     * replacement Start. Matching InProgress must not become a stale cancel. */
    if (client->lastLaunchId)
        client->lastLaunchHasTicket = true;
    IHS_BaseUnlock(&client->base);
}

/* Sender failure policy is shared by immediate responses and due Updates.
 * No failure callback runs on the timer hot path: cleanup observes retirement. */
static bool SendCurrentRequest(IHS_StreamingState *state) {
    IHS_Client *client = state->client;
    bool ok = state->authorizing
        ? IHS_ClientSendAuthorizationRequest(client, &state->host, &state->authorization, state->requestId)
        : SendStreamingRequest(state);
    if (!ok) {
        state->terminal = state->timerFailed = true;
        if (state->authorizing) IHS_ClientAuthorizationSendCancel(client, &state->host);
        else SendCancel(client, &state->host, state->requestId);
        ClearLaunch(state);
        return false;
    }
    int64_t sent = (int64_t)time(NULL);
    state->hasSent = true;
    state->nextSendWallSecond = (int64_t)((uint64_t)sent + (state->authorizing ? 3u : 1u));
    return true;
}
static bool StreamingAdvance(IHS_StreamingState *state, bool initialOnly) {
    if (state->terminal || state->ticket || (initialOnly && state->hasSent)) return true;
    IHS_Client *client = state->client;
    /* Timer visitor -> Base is the existing lock order. Hold Base through the
     * due send so a new RX batch cannot start between this check and send. */
    IHS_BaseLock(&client->base);
    bool ready = !client->base.interrupted && !client->receiveBatchActive &&
                 !client->authorizationCount && !client->authorizationDispatching;
    bool fresh = state->receiveGeneration != client->receiveGeneration;
    bool ok = true;
    if (ready && (!state->hasSent || fresh)) {
        state->receiveGeneration = client->receiveGeneration;
        if ((int64_t)time(NULL) >= state->nextSendWallSecond)
            ok = SendCurrentRequest(state);
    }
    IHS_BaseUnlock(&client->base);
    return ok;
}
static uint64_t StreamingRequestTimer(int runCount, void *context) {
    (void)runCount;
    IHS_StreamingState *state = context;
    if (state->terminal) return 0;
    if (atomic_load_explicit(&state->client->deferAuthorization, memory_order_acquire)) return 1;
    /* First send may precede a poll; retries belong to completed RX updates. */
    return StreamingAdvance(state, true) ? 1 : 0;
}
static void StreamingUpdateVisit(IHS_TimerTask *task, void *context) {
    (void)context;
    if (!StreamingAdvance(IHS_TimerTaskGetContext(task), false)) IHS_TimerTaskStop(task);
}
void IHS_ClientStreamingUpdate(IHS_Client *client) {
    IHS_TimerTaskVisitOwned(client->timers, &client->taskHandles.streaming, StreamingUpdateVisit, NULL);
}

static void StreamingAuthorizationVisit(IHS_TimerTask *task, void *context) {
    StreamingResponseContext *response = context;
    IHS_Client *client = response->client;
    IHS_StreamingState *state = IHS_TimerTaskGetContext(task);
    if (response->header->msg_type != k_ERemoteDeviceAuthorizationResponse || !response->message) return;
    CMsgRemoteDeviceAuthorizationResponse *resp = (void *)response->message;
    if (!IHS_ClientMatchesHost(client, &state->host, response->address, response->header))
        return;
    if (state->terminal || state->ticket || !state->authorizing) {
        if (resp->has_auth_key)
            IHS_ClientAuthorizationConfirm(client, *response->address, response->header->client_id,
                                           k_ERemoteDeviceAuthorizationCanceled);
        return;
    }
    IHS_AuthorizationExchangeOutcome outcome = IHS_AuthorizationExchangeAccepted;
    if (resp->result == k_ERemoteDeviceAuthorizationSuccess && resp->has_auth_key) {
        outcome = IHS_ClientAuthorizationExchange(client, &state->host, &state->authorization, resp,
                                                  response->address);
        if (outcome == IHS_AuthorizationExchangeWaiting) return;
    } else if (resp->result == k_ERemoteDeviceAuthorizationInProgress) return;
    if (resp->result != k_ERemoteDeviceAuthorizationSuccess ||
        outcome == IHS_AuthorizationExchangeExhausted || outcome == IHS_AuthorizationExchangeResourceFailure) {
        state->terminal = true;
        IHS_TimerTaskStop(task);
        if (outcome != IHS_AuthorizationExchangeExhausted) IHS_ClientAuthorizationSendCancel(client, &state->host);
        ClearLaunch(state);
        IHS_AuthorizationResult result = outcome == IHS_AuthorizationExchangeExhausted ? IHS_AuthorizationDenied :
            outcome == IHS_AuthorizationExchangeResourceFailure ? IHS_AuthorizationFailed : (IHS_AuthorizationResult)resp->result;
        NotifyAuthorizationFailure(client, &state->host, result);
        return;
    }
    SetAuthorizing(state, false);
    if (!SendCurrentRequest(state)) {
        IHS_TimerTaskStop(task);
        return;
    }
    NotifyAuthorization(state, NULL, resp->steamid);
}
bool IHS_ClientStreamingAuthorizationCallback(IHS_Client *client, const IHS_SocketAddress *address,
        CMsgRemoteClientBroadcastHeader *header, ProtobufCMessage *message) {
    StreamingResponseContext context = {client, header, message, address};
    return IHS_TimerTaskVisitOwned(client->timers, &client->taskHandles.streaming,
                                   StreamingAuthorizationVisit, &context);
}
