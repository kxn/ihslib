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
#include "pubkeys.h"
#include "crypto.h"
#include "authorization_flow.h"

#include <string.h>
#include <stdlib.h>
#include <time.h>

typedef struct IHS_AuthorizationState {
    IHS_Client *client;
    IHS_HostInfo host;
    IHS_AuthorizationFlow flow;
    uint32_t requestId;
    bool terminal;
    bool timerFailed;
    bool service;
    int64_t nextSendWallSecond;
} IHS_AuthorizationState;

static uint64_t AuthorizationRequestTimer(int runCount, void *data);

static void AuthorizationRequestCleanup(void *data);

static void AuthorizationConfigureTicket(IHS_Client *client, IHS_AuthorizationFlow *flow,
                                         const uint8_t secret[32],
                                         CMsgRemoteDeviceAuthorizationRequest__CKeyEscrowTicket *ticket);

static void AuthorizationKeyExchange(IHS_Client *client, IHS_AuthorizationState *state,
                                     IHS_TimerTask *task,
                                     const CMsgRemoteDeviceAuthorizationResponse *resp,
                                     const IHS_SocketAddress *replySource, uint64_t replyClientId);

static void ClearServicePairing(IHS_Client *client) {
    /* Original state3 Stop still uses the old selected identity/request ID and
     * attempts a streaming cancel when GetServerInfo (or its hint) is usable.
     * In particular, a usable hint does not require a nonzero request ID. */
    IHS_BaseLock(&client->base);
    IHS_HostInfo host = client->lastLaunchHost;
    uint32_t id = client->lastLaunchId;
    bool active = client->servicePairing;
    IHS_BaseUnlock(&client->base);
    const IHS_ClientRoutingCallbacks *route = client->callbacks.routing;
    IHS_HostInfo visible;
    if (active && route && route->host &&
        route->host(client, host.clientId, &visible, client->callbackContexts.routing)) {
        CMsgRemoteDeviceStreamingCancelRequest cancel =
            CMSG_REMOTE_DEVICE_STREAMING_CANCEL_REQUEST__INIT;
        cancel.request_id = id;
        IHS_ClientSendControl(client, &host, NULL, k_ERemoteDeviceStreamingCancelRequest,
                              &cancel.base);
    }
    IHS_BaseLock(&client->base);
    client->servicePairing = false;
    client->serviceAwaitingPin = false;
    client->lastLaunchId = 0;
    memset(&client->lastLaunchHost, 0, sizeof(client->lastLaunchHost));
    client->lastLaunchAuthorizing = client->lastLaunchHasTicket = false;
    IHS_BaseUnlock(&client->base);
}
bool IHS_ClientAuthorizationServiceActive(IHS_Client *client) {
    IHS_BaseLock(&client->base);
    bool active = client->servicePairing;
    IHS_BaseUnlock(&client->base);
    return active;
}
unsigned IHS_ClientHandshakePhase(IHS_Client *client) {
    IHS_BaseLock(&client->base);
    unsigned phase = client->servicePairing          ? (client->serviceAwaitingPin ? 2 : 3)
                     : !client->lastLaunchId         ? 1
                     : client->lastLaunchHasTicket   ? 6
                     : client->lastLaunchAuthorizing ? 4
                                                     : 5;
    IHS_BaseUnlock(&client->base);
    return phase;
}
bool IHS_ClientHandshakeFail(IHS_Client *client, IHS_AuthorizationResult result) {
    if (IHS_ClientHandshakePhase(client) == 1)
        return false;
    IHS_BaseLock(&client->base);
    IHS_HostInfo host = client->lastLaunchHost;
    bool service = client->servicePairing;
    IHS_BaseUnlock(&client->base);
    if (client->callbacks.authorization && client->callbacks.authorization->failed)
        client->callbacks.authorization->failed(client, &host, result,
                                                client->callbackContexts.authorization);
    if (service)
        IHS_ClientAuthorizationCancel(client);
    else {
        IHS_ClientStreamingCancel(client);
        IHS_ClientAuthorizationCancel(client);
    }
    return true;
}
bool IHS_ClientAuthorizationAwaitService(IHS_Client *client) {
    IHS_TimerTaskStopOwned(client->timers, &client->taskHandles.authorization);
    IHS_TimerTaskStopOwned(client->timers, &client->taskHandles.streaming);
    IHS_AuthorizationState *state = calloc(1, sizeof(*state));
    if (!state)
        return false;
    state->client = client;
    state->service = true;
    IHS_BaseLock(&client->base);
    state->host = client->lastLaunchHost;
    state->requestId = client->lastLaunchId;
    client->servicePairing = client->serviceAwaitingPin = true;
    IHS_BaseUnlock(&client->base);
    if (IHS_TimerTaskStartOwned(client->timers, &client->taskHandles.authorization,
                                AuthorizationRequestTimer, AuthorizationRequestCleanup, 0, state))
        return true;
    ClearServicePairing(client);
    free(state);
    return false;
}

void IHS_ClientAuthorizationSendCancel(IHS_Client *client, const IHS_HostInfo *host) {
    CMsgRemoteDeviceAuthorizationCancelRequest request = CMSG_REMOTE_DEVICE_AUTHORIZATION_CANCEL_REQUEST__INIT;
    IHS_ClientSendControl(client, host, NULL, k_ERemoteDeviceAuthorizationCancelRequest,
                          (ProtobufCMessage *)&request);
}

static void AuthorizationFail(IHS_Client *client, IHS_AuthorizationState *state,
                              IHS_TimerTask *task, IHS_AuthorizationResult result) {
    state->terminal = true;
    IHS_TimerTaskStop(task);
    if (client->callbacks.authorization && client->callbacks.authorization->failed)
        client->callbacks.authorization->failed(client, &state->host, result,
                                                client->callbackContexts.authorization);
    if (state->service)
        ClearServicePairing(client);
    else
        IHS_ClientAuthorizationSendCancel(client, &state->host);
}
static void AuthorizationExhausted(void *opaque) {
    IHS_TimerTask *task = opaque;
    IHS_AuthorizationState *state = IHS_TimerTaskGetContext(task);
    AuthorizationFail(state->client, state, task, IHS_AuthorizationDenied);
}

void IHS_ClientAuthorizationConfirm(IHS_Client *client, IHS_SocketAddress address,
                                    uint64_t clientId, ERemoteDeviceAuthorizationResult result) {
    CMsgRemoteDeviceAuthorizationConfirmed confirmed = CMSG_REMOTE_DEVICE_AUTHORIZATION_CONFIRMED__INIT;
    confirmed.result = result;
    IHS_HostInfo peer = {.clientId = clientId};
    IHS_ClientSendControl(client, &peer, &address, k_ERemoteDeviceAuthorizationConfirmed,
                          &confirmed.base);
}

bool IHS_ClientAuthorizationRequest(IHS_Client *client, const IHS_HostInfo *host, const char *pin) {
    IHS_AuthorizationState *state = calloc(1, sizeof(IHS_AuthorizationState));
    if (!state) return false;
    state->client = client;
    state->host = *host;
    state->requestId = IHS_CryptoRandomUInt32();
    if (!IHS_ClientAuthorizationFlowInit(&state->flow, pin)) {
        free(state);
        return false;
    }
    IHS_BaseLock(&client->base);
    client->servicePairing = false;
    client->serviceAwaitingPin = false;
    IHS_BaseUnlock(&client->base);
    if (!IHS_TimerTaskStartOwned(client->timers, &client->taskHandles.authorization,
            AuthorizationRequestTimer, AuthorizationRequestCleanup, 0, state)) {
        IHS_AuthorizationKeyExchangeClear(&state->flow.keyExchange);
        free(state);
        return false;
    }
    return true;
}

typedef struct {
    IHS_Client *client;
    bool canceled;
} AuthorizationCancelContext;
static void AuthorizationCancelVisit(IHS_TimerTask *task, void *context) {
    AuthorizationCancelContext *cancel = context;
    IHS_Client *client = cancel->client;
    if (!task) {
        /* A failed service producer has retired its private task while state2
         * still owns notification/Stop. An empty slot does not mean Idle. */
        cancel->canceled = IHS_ClientAuthorizationServiceActive(client);
        if (cancel->canceled)
            ClearServicePairing(client);
        return;
    }
    cancel->canceled = true;
    IHS_AuthorizationState *state = IHS_TimerTaskGetContext(task);
    CMsgRemoteDeviceAuthorizationCancelRequest request = CMSG_REMOTE_DEVICE_AUTHORIZATION_CANCEL_REQUEST__INIT;
    IHS_HostInfo host = state->host;
    bool service = state->service;
    /* Retire the owned slot synchronously so a replacement can start before
     * the next timer tick. Only copied values may be used after cleanup. */
    IHS_TimerTaskStopOwned(client->timers, &client->taskHandles.authorization);
    if (service)
        ClearServicePairing(client);
    else
        IHS_ClientSendControl(client, &host, NULL, k_ERemoteDeviceAuthorizationCancelRequest,
                              &request.base);
}

bool IHS_ClientAuthorizationCancel(IHS_Client *client) {
    AuthorizationCancelContext context = {.client = client};
    IHS_TimerTaskVisitSlot(client->timers, &client->taskHandles.authorization,
                           AuthorizationCancelVisit, &context);
    return context.canceled;
}

typedef struct {
    IHS_Client *client;
    CMsgRemoteClientBroadcastHeader *header;
    ProtobufCMessage *message;
    const IHS_SocketAddress *address;
} AuthorizationResponseContext;

static void AuthorizationResponseVisit(IHS_TimerTask *task, void *context) {
    AuthorizationResponseContext *response = context;
    IHS_Client *client = response->client;
    CMsgRemoteClientBroadcastHeader *header = response->header;
    ProtobufCMessage *message = response->message;
    IHS_AuthorizationState *current = IHS_TimerTaskGetContext(task);
    if (!current->service && !client->callbacks.routing &&
        !IHS_ClientMatchesHost(client, &current->host, response->address, header))
        return;
    if (!header->client_id)
        return;
    if (header->msg_type == k_ERemoteDeviceAuthorizationConfirmed) {
        CMsgRemoteDeviceAuthorizationConfirmed *confirmed =
                (CMsgRemoteDeviceAuthorizationConfirmed *) message;
        IHS_ClientLog(client, IHS_LogLevelInfo, "Authorization",
                      "Authorization confirmed result=%d",
                      confirmed ? (int) confirmed->result : -1);
        return;
    }
    if (header->msg_type != k_ERemoteDeviceAuthorizationResponse) {
        return;
    }
    IHS_AuthorizationState *state = IHS_TimerTaskGetContext(task);
    CMsgRemoteDeviceAuthorizationResponse *resp = (CMsgRemoteDeviceAuthorizationResponse *) message;
    if (resp == NULL) {
        IHS_ClientLog(client, IHS_LogLevelWarn, "Authorization", "Malformed authorization response");
        return;
    }
    IHS_ClientLog(client, IHS_LogLevelInfo, "Authorization",
                  "Authorization response result=%d steamid=%llu auth_key=%zu device_token=%zu",
                  (int) resp->result, (unsigned long long) resp->steamid,
                  resp->has_auth_key ? resp->auth_key.len : 0,
                  resp->has_device_token ? resp->device_token.len : 0);
    if (state->terminal) {
        if (resp->has_auth_key)
            IHS_ClientAuthorizationConfirm(client, *response->address, header->client_id,
                                           k_ERemoteDeviceAuthorizationCanceled);
        return;
    }
    IHS_BaseLock(&client->base);
    bool awaitingPin = state->service && client->serviceAwaitingPin;
    IHS_BaseUnlock(&client->base);
    if (awaitingPin) {
        if (resp->has_auth_key)
            IHS_ClientAuthorizationConfirm(client, *response->address, header->client_id,
                                           k_ERemoteDeviceAuthorizationCanceled);
        return;
    }
    /* Official Android checks result before auth_key (protocol RE §128).
     * Failed key calculations are confirmed but keep the transaction alive. */
    if (resp->result == k_ERemoteDeviceAuthorizationSuccess && resp->has_steamid &&
        client->callbacks.authorization && client->callbacks.authorization->identity)
        client->callbacks.authorization->identity(client, header->client_id, resp->steamid,
                                                  client->callbackContexts.authorization);
    if (resp->result == k_ERemoteDeviceAuthorizationSuccess && resp->has_auth_key) {
        AuthorizationKeyExchange(client, state, task, resp, response->address, header->client_id);
        return;
    }
    if (state->service && resp->result == k_ERemoteDeviceAuthorizationSuccess) {
        /* State3's no-key compatibility branch persists the current temporary
         * secret. State4's no-key branch deliberately does not (Core @79d7f8). */
        uint8_t secret[32];
        IHS_BaseGetSecretKey(&client->base, secret);
        IHS_HostInfo peer = {.clientId = header->client_id, .address = *response->address};
        if (client->callbacks.authorization && client->callbacks.authorization->persist)
            client->callbacks.authorization->persist(client, &peer, resp->steamid, secret,
                                                     client->callbackContexts.authorization);
        memset(secret, 0, sizeof(secret));
    }
    if (resp->result == k_ERemoteDeviceAuthorizationSuccess &&
        client->callbacks.authorization && client->callbacks.authorization->subscribe)
        client->callbacks.authorization->subscribe(client, resp->steamid,
                                                   client->callbackContexts.authorization);
    if (!state->service && client->callbacks.routing && header->client_id != state->host.clientId) {
        if (resp->has_auth_key)
            IHS_ClientAuthorizationConfirm(client, *response->address, header->client_id,
                                           k_ERemoteDeviceAuthorizationCanceled);
        return;
    }
    switch (resp->result) {
        case k_ERemoteDeviceAuthorizationInProgress:
            if (state->service) {
                AuthorizationFail(client, state, task, IHS_AuthorizationInProgress);
                return;
            }
            if (client->callbacks.authorization && client->callbacks.authorization->progress) {
                client->callbacks.authorization->progress(client, &state->host, client->callbackContexts.authorization);
            }
            return;
        case k_ERemoteDeviceAuthorizationSuccess:
            state->terminal = true;
            IHS_TimerTaskStop(task);
            if (client->callbacks.authorization && client->callbacks.authorization->success) {
                IHS_HostInfo peer = state->host;
                if (state->service) {
                    peer.clientId = header->client_id;
                    peer.address = *response->address;
                }
                client->callbacks.authorization->success(client, &peer, resp->steamid,
                                                         client->callbackContexts.authorization);
            }
            if (state->service)
                ClearServicePairing(client);
            return;
        default:
            AuthorizationFail(client, state, task, (IHS_AuthorizationResult)resp->result);
            return;
    }
}


bool IHS_ClientAuthorizationPubKey(IHS_Client *client, IHS_SteamUniverse universe, uint8_t *key, size_t *keyLen) {
    IHS_UNUSED(client);
    switch (universe) {
        case IHS_SteamUniversePublic:
            if (*keyLen < sizeof(IHS_AuthorizationPubKeyPublic)) return false;
            memcpy(key, IHS_AuthorizationPubKeyPublic, sizeof(IHS_AuthorizationPubKeyPublic));
            *keyLen = sizeof(IHS_AuthorizationPubKeyPublic);
            break;
        case IHS_SteamUniverseBeta:
            if (*keyLen < sizeof(IHS_AuthorizationPubKeyBeta)) return false;
            memcpy(key, IHS_AuthorizationPubKeyBeta, sizeof(IHS_AuthorizationPubKeyBeta));
            *keyLen = sizeof(IHS_AuthorizationPubKeyBeta);
            break;
        case IHS_SteamUniverseInternal:
        case IHS_SteamUniverseDev:
            if (*keyLen < sizeof(IHS_AuthorizationPubKeyDev)) return false;
            memcpy(key, IHS_AuthorizationPubKeyDev, sizeof(IHS_AuthorizationPubKeyDev));
            *keyLen = sizeof(IHS_AuthorizationPubKeyDev);
            break;
        default:
            return false;
    }
    return true;
}

void IHS_ClientAuthorizationCallback(IHS_Client *client, const IHS_SocketAddress *address,
                                     CMsgRemoteClientBroadcastHeader *header, ProtobufCMessage *message) {
    AuthorizationResponseContext context = {client, header, message, address};
    bool present = IHS_TimerTaskVisitOwned(client->timers, &client->taskHandles.authorization,
                                           AuthorizationResponseVisit, &context);
    /* Outside an authorization transaction the original handler cancels a
     * response carrying auth_key, without deriving or notifying success. */
    if (!present && IHS_ClientStreamingAuthorizationCallback(client, address, header, message)) return;
    if (!present && header->msg_type == k_ERemoteDeviceAuthorizationResponse && message &&
        ((CMsgRemoteDeviceAuthorizationResponse *)message)->has_auth_key)
        IHS_ClientAuthorizationConfirm(client, *address, header->client_id,
                                       k_ERemoteDeviceAuthorizationCanceled);
}

static uint64_t AuthorizationRequestTimer(int runCount, void *data) {
    IHS_AuthorizationState *state = data;
    IHS_Client *client = state->client;
    if (client->polled && IHS_ClientDispatchOwner != client) return 100;
    if (state->service)
        return 1; /* Service owns publication; never retry as host cancel/request. */
    int64_t now = client->updateClockActive ? client->updateClock : (int64_t)time(NULL);
    /* Original manager Update compares whole wall seconds, including clock
     * changes. Polled clients advance only inside the actor's Update. */
    if (runCount && now < state->nextSendWallSecond) return 1;
    if (!IHS_ClientSendAuthorizationRequest(client, &state->host, &state->flow, state->requestId)) {
        state->terminal = state->timerFailed = true;
        IHS_ClientAuthorizationSendCancel(client, &state->host);
        return 0;
    }
    int64_t sentAt = (int64_t)time(NULL);
    state->nextSendWallSecond = (int64_t)((uint64_t)sentAt + 3);
    return 1;
}
static void AuthorizationUpdateVisit(IHS_TimerTask *task, void *unused) {
    (void)unused;
    if (!AuthorizationRequestTimer(1, IHS_TimerTaskGetContext(task))) IHS_TimerTaskStop(task);
}
void IHS_ClientAuthorizationUpdate(IHS_Client *client) {
    IHS_TimerTaskVisitOwned(client->timers, &client->taskHandles.authorization,
                            AuthorizationUpdateVisit, NULL);
}

bool IHS_ClientAuthorizationFlowInit(IHS_AuthorizationFlow *flow, const char *pin) {
    memset(flow, 0, sizeof(*flow));
    strncpy(flow->pin, pin, sizeof(flow->pin) - 1);
    if (IHS_AuthorizationKeyExchangeInit(&flow->keyExchange, flow->pin)) return true;
    IHS_AuthorizationKeyExchangeClear(&flow->keyExchange);
    return false;
}

typedef struct AuthorizationRequestStorage {
    CMsgRemoteDeviceAuthorizationRequest message;
    uint8_t token[32], encrypted[2048], authKey[32];
} AuthorizationRequestStorage;

static bool CreateAuthorizationRequest(IHS_Client *client, IHS_SteamUniverse universe,
                                       IHS_AuthorizationFlow *flow, uint32_t requestId,
                                       AuthorizationRequestStorage *storage) {
    uint8_t pubKey[384], secret[32], ticketBytes[2048];
    size_t pubKeyLen = sizeof(pubKey), encryptedLen = sizeof(storage->encrypted);
    if (!IHS_ClientAuthorizationPubKey(client, universe, pubKey, &pubKeyLen))
        return false;
    IHS_BaseGetSecretKey(&client->base, secret);
    CMsgRemoteDeviceAuthorizationRequest__CKeyEscrowTicket ticket =
        CMSG_REMOTE_DEVICE_AUTHORIZATION_REQUEST__CKEY_ESCROW__TICKET__INIT;
    AuthorizationConfigureTicket(client, flow, secret, &ticket);
    size_t ticketLen = protobuf_c_message_get_packed_size(&ticket.base);
    if (ticketLen > sizeof(ticketBytes)) {
        memset(secret, 0, sizeof(secret));
        return false;
    }
    protobuf_c_message_pack(&ticket.base, ticketBytes);
    /* Original BCreateAuthorizationRequest @79c848 calls RSA before the
     * @79c860 device token. A failed token must not skip the RSA caller. */
    int crypto = IHS_CryptoRSAEncrypt(ticketBytes, ticketLen, pubKey, pubKeyLen, storage->encrypted,
                                      &encryptedLen);
    memset(ticketBytes, 0, sizeof(ticketBytes));
    if (crypto) {
        memset(secret, 0, sizeof(secret));
        return false;
    }
    bool tokenReady = IHS_ClientCreateDeviceToken(client, secret, storage->token);
    memset(secret, 0, sizeof(secret));
    if (!tokenReady)
        return false;
    IHS_AuthorizationKeyExchangeMaskedKey(&flow->keyExchange, storage->authKey);
    CMsgRemoteDeviceAuthorizationRequest *request = &storage->message;
    cmsg_remote_device_authorization_request__init(request);
    request->device_name = client->base.deviceName;
    request->device_token = (ProtobufCBinaryData){sizeof(storage->token), storage->token};
    request->encrypted_request = (ProtobufCBinaryData){encryptedLen, storage->encrypted};
    request->has_auth_key = request->has_request_id = true;
    request->auth_key = (ProtobufCBinaryData){sizeof(storage->authKey), storage->authKey};
    request->request_id = requestId;
    return true;
}

bool IHS_ClientSendAuthorizationRequest(IHS_Client *client, const IHS_HostInfo *host,
                                       IHS_AuthorizationFlow *flow, uint32_t requestId) {
    return IHS_ClientTryAuthorizationRequest(client, host, flow, requestId) ==
           IHS_ClientRequestFailureNone;
}

IHS_ClientRequestFailure IHS_ClientTryAuthorizationRequest(IHS_Client *client,
        const IHS_HostInfo *host, IHS_AuthorizationFlow *flow, uint32_t requestId) {
    IHS_HostInfo current = *host;
    if (client->callbacks.routing && client->callbacks.routing->host &&
        !client->callbacks.routing->host(client, host->clientId, &current,
                                         client->callbackContexts.routing))
        return IHS_ClientRequestFailureUnavailable;
    AuthorizationRequestStorage storage;
    bool ok = CreateAuthorizationRequest(client, current.universe, flow, requestId, &storage);
    if (ok) {
        bool sent = IHS_ClientSendControl(client, &current, NULL, k_ERemoteDeviceAuthorizationRequest,
                                          &storage.message.base);
        /* Official producer calls void SendMessage; only body construction
         * failure stops it. Retain the legacy un-routed API's bool contract. */
        if (!client->callbacks.routing)
            ok = sent;
    }
    memset(&storage, 0, sizeof(storage));
    return ok ? IHS_ClientRequestFailureNone : IHS_ClientRequestFailureGeneric;
}

bool IHS_ClientAuthorizationPrepareService(IHS_Client *client, IHS_SteamUniverse universe,
                                           const char *pin, uint8_t **body, size_t *size) {
    *body = NULL;
    *size = 0;
    if (!pin || strlen(pin) != 4)
        return false;
    IHS_BaseLock(&client->base);
    uint32_t currentId = client->lastLaunchId;
    IHS_HostInfo currentHost = client->lastLaunchHost;
    bool previousService = client->servicePairing;
    bool previousAwaitingPin = client->serviceAwaitingPin;
    bool previousAuthorizing = client->lastLaunchAuthorizing;
    bool previousTicket = client->lastLaunchHasTicket;
    IHS_BaseUnlock(&client->base);
    /* Replacement does not erase the manager-lifetime public key/result cache. */
    IHS_TimerTaskStopOwned(client->timers, &client->taskHandles.authorization);
    IHS_TimerTaskStopOwned(client->timers, &client->taskHandles.streaming);
    IHS_AuthorizationState *state = calloc(1, sizeof(*state));
    if (!state)
        return false;
    state->client = client;
    state->service = true;
    state->host.universe = universe;
    state->requestId = currentId;
    bool ok = IHS_ClientAuthorizationFlowInit(&state->flow, pin);
    uint8_t secret[32] = {0};
    while (ok) {
        /* Official OnPairingStarted @79f054 retries the entire random block
         * until byte zero is nonzero. A provider failure is a local guard. */
        ok = IHS_CryptoRandomBytes(secret, sizeof(secret)) == 0;
        if (!ok || secret[0])
            break;
    }
    if (ok)
        IHS_BaseSetSecretKey(&client->base, secret);
    memset(secret, 0, sizeof(secret));
    AuthorizationRequestStorage storage;
    if (ok)
        ok = CreateAuthorizationRequest(client, universe, &state->flow, currentId, &storage);
    if (ok) {
        *size = protobuf_c_message_get_packed_size(&storage.message.base);
        *body = malloc(*size);
        ok = *body != NULL;
        if (ok)
            protobuf_c_message_pack(&storage.message.base, *body);
    }
    memset(&storage, 0, sizeof(storage));
    if (ok) {
        IHS_BaseLock(&client->base);
        client->lastLaunchId = currentId;
        client->lastLaunchHost = currentHost;
        client->lastLaunchHasTicket = client->lastLaunchAuthorizing = false;
        client->servicePairing = true;
        client->serviceAwaitingPin = false;
        IHS_BaseUnlock(&client->base);
        ok = IHS_TimerTaskStartOwned(client->timers, &client->taskHandles.authorization,
                                     AuthorizationRequestTimer, AuthorizationRequestCleanup, 0,
                                     state);
    }
    if (!ok) {
        /* OnPairingStarted commits state3 only after a complete body. Keep the
         * old phase2/ID/target for the caller's failure notification and Stop;
         * clearing here would turn that Stop into an idle no-op. */
        IHS_BaseLock(&client->base);
        client->lastLaunchId = currentId;
        client->lastLaunchHost = currentHost;
        client->servicePairing = previousService;
        client->serviceAwaitingPin = previousAwaitingPin;
        client->lastLaunchAuthorizing = previousAuthorizing;
        client->lastLaunchHasTicket = previousTicket;
        IHS_BaseUnlock(&client->base);
        IHS_AuthorizationKeyExchangeClear(&state->flow.keyExchange);
        free(state);
        free(*body);
        *body = NULL;
        *size = 0;
    }
    return ok;
}

static void AuthorizationConfigureTicket(IHS_Client *client, IHS_AuthorizationFlow *flow,
                                         const uint8_t secret[32],
                                         CMsgRemoteDeviceAuthorizationRequest__CKeyEscrowTicket *ticket) {
    ticket->has_password = true;
    ticket->password.len = strlen(flow->pin);
    ticket->password.data = (uint8_t *) flow->pin;

    ticket->has_identifier = true;
    ticket->identifier = client->base.deviceId;

    ticket->has_payload = true;
    ticket->payload.len = sizeof(client->base.secretKey);
    ticket->payload.data = (uint8_t *)secret;

    ticket->has_usage = true;
    ticket->usage = k_EKeyEscrowUsageStreamingDevice;

    ticket->device_name = client->base.deviceName;
    ticket->device_model = client->base.deviceModel;
    ticket->device_serial = client->base.deviceSerial;
    ticket->has_device_provisioning_id = true;
    ticket->device_provisioning_id = client->base.deviceProvisioningId;
}

IHS_AuthorizationExchangeOutcome IHS_ClientAuthorizationExchange(
    IHS_Client *client, const IHS_HostInfo *host, IHS_AuthorizationFlow *flow,
    const CMsgRemoteDeviceAuthorizationResponse *resp, const IHS_SocketAddress *replySource,
    uint64_t replyClientId, void (*stopExhausted)(void *), void *stopContext) {
    IHS_AuthorizationCache *cache = &client->authorizationCache;
    size_t count = resp->auth_key.len;
    IHS_BaseLock(&client->base);
    bool cached = count == cache->keyLen &&
                  (!count || !memcmp(resp->auth_key.data, cache->key, count));
    ERemoteDeviceAuthorizationResult cachedResult = cache->result;
    IHS_BaseUnlock(&client->base);
    if (cached) {
        IHS_ClientAuthorizationConfirm(client, *replySource, replyClientId, cachedResult);
        return IHS_AuthorizationExchangeWaiting; /* Cache never advances twice. */
    }
    uint8_t *key = count ? malloc(count) : NULL;
    if (count && !key) {
        /* Local allocation failure guard, not an inferred protocol result. */
        return IHS_AuthorizationExchangeResourceFailure;
    }
    if (count) memcpy(key, resp->auth_key.data, count);
    uint8_t secretKey[IHS_CRYPTO_SHA256_SIZE] = {0};
    bool exhausted = flow->keyExchangeAttempts++ >= 3;
    bool ok = !exhausted && resp->has_device_token &&
              IHS_AuthorizationKeyExchangeComplete(&flow->keyExchange, resp->auth_key.data,
                                                   count, resp->device_token.data,
                                                   resp->device_token.len, client->base.deviceId,
                                                   secretKey);
    /* Original BHandle stops/notifies before the caller replaces the public
     * cache and emits Confirmed. Stop retains the current secret. */
    if (exhausted && stopExhausted)
        stopExhausted(stopContext);
    if (ok) {
        IHS_BaseSetSecretKey(&client->base, secretKey);
        IHS_HostInfo peer = replyClientId == host->clientId ? *host : (IHS_HostInfo){0};
        peer.clientId = replyClientId;
        peer.address = *replySource;
        if (client->callbacks.authorization && client->callbacks.authorization->persist)
            client->callbacks.authorization->persist(client, &peer, resp->steamid, secretKey,
                                                     client->callbackContexts.authorization);
    }
    memset(secretKey, 0, sizeof(secretKey));
    IHS_BaseLock(&client->base);
    free(cache->key);
    cache->key = key;
    cache->keyLen = count;
    cache->result = ok ? k_ERemoteDeviceAuthorizationSuccess : k_ERemoteDeviceAuthorizationDenied;
    IHS_BaseUnlock(&client->base);
    IHS_ClientAuthorizationConfirm(client, *replySource, replyClientId, cache->result);
    if (exhausted)
        return IHS_AuthorizationExchangeExhausted;
    return ok ? IHS_AuthorizationExchangeAccepted : IHS_AuthorizationExchangeWaiting;
}

static void AuthorizationKeyExchange(IHS_Client *client, IHS_AuthorizationState *state,
                                     IHS_TimerTask *task,
                                     const CMsgRemoteDeviceAuthorizationResponse *resp,
                                     const IHS_SocketAddress *replySource, uint64_t replyClientId) {
    IHS_AuthorizationExchangeOutcome outcome =
        IHS_ClientAuthorizationExchange(client, &state->host, &state->flow, resp, replySource,
                                        replyClientId, AuthorizationExhausted, task);
    if (outcome == IHS_AuthorizationExchangeWaiting ||
        outcome == IHS_AuthorizationExchangeExhausted)
        return;
    if (outcome == IHS_AuthorizationExchangeResourceFailure) {
        AuthorizationFail(client, state, task, IHS_AuthorizationFailed);
        return;
    }
    if (client->callbacks.authorization && client->callbacks.authorization->subscribe)
        client->callbacks.authorization->subscribe(client, resp->steamid,
                                                   client->callbackContexts.authorization);
    if (!state->service && outcome == IHS_AuthorizationExchangeAccepted &&
        client->callbacks.routing && replyClientId != state->host.clientId) {
        IHS_ClientAuthorizationConfirm(client, *replySource, replyClientId,
                                       k_ERemoteDeviceAuthorizationCanceled);
        return;
    }
    state->terminal = true;
    IHS_TimerTaskStop(task);
    IHS_HostInfo peer = state->host;
    if (state->service) {
        peer.clientId = replyClientId;
        peer.address = *replySource;
    }
    if (client->callbacks.authorization && client->callbacks.authorization->success)
        client->callbacks.authorization->success(client, &peer, resp->steamid,
                                                 client->callbackContexts.authorization);
    if (state->service)
        ClearServicePairing(client);
}

static void AuthorizationRequestCleanup(void *data) {
    IHS_AuthorizationState *state = data;
    IHS_AuthorizationKeyExchangeClear(&state->flow.keyExchange);
    /* Most-recent public auth_key/result outlive this private request material. */
    if (state->timerFailed && state->client->callbacks.authorization &&
        state->client->callbacks.authorization->failed)
        state->client->callbacks.authorization->failed(state->client, &state->host,
            IHS_AuthorizationFailed, state->client->callbackContexts.authorization);
    free(data);
}
