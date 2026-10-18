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
#include "authorization_kx.h"

#include <string.h>
#include <stdlib.h>
#include <time.h>

typedef struct IHS_AuthorizationState {
    IHS_Client *client;
    IHS_HostInfo host;
    char deviceName[64];
    char pin[16];
    uint32_t requestId;
    IHS_AuthorizationKeyExchange keyExchange;
    unsigned keyExchangeAttempts;
    bool terminal;
    bool timerFailed;
    int64_t nextSendWallSecond;
} IHS_AuthorizationState;

static uint64_t AuthorizationRequestTimer(int runCount, void *data);

static void AuthorizationRequestCleanup(void *data);

static void AuthorizationConfigureTicket(IHS_Client *client, IHS_AuthorizationState *state,
                                         const uint8_t secret[32],
                                         CMsgRemoteDeviceAuthorizationRequest__CKeyEscrowTicket *ticket);

static void AuthorizationKeyExchange(IHS_Client *client, IHS_AuthorizationState *state,
                                     IHS_TimerTask *task,
                                     const CMsgRemoteDeviceAuthorizationResponse *resp);

static void AuthorizationSendCancel(IHS_Client *client, const IHS_HostInfo *host) {
    CMsgRemoteDeviceAuthorizationCancelRequest request = CMSG_REMOTE_DEVICE_AUTHORIZATION_CANCEL_REQUEST__INIT;
    IHS_ClientSend(client, host->address, k_ERemoteDeviceAuthorizationCancelRequest, (ProtobufCMessage *)&request);
}

static void AuthorizationFail(IHS_Client *client, IHS_AuthorizationState *state,
                              IHS_TimerTask *task, IHS_AuthorizationResult result) {
    state->terminal = true;
    IHS_TimerTaskStop(task);
    AuthorizationSendCancel(client, &state->host);
    if (client->callbacks.authorization && client->callbacks.authorization->failed)
        client->callbacks.authorization->failed(client, &state->host, result,
                                                client->callbackContexts.authorization);
}

static void AuthorizationConfirm(IHS_Client *client, IHS_SocketAddress address,
                                 ERemoteDeviceAuthorizationResult result) {
    CMsgRemoteDeviceAuthorizationConfirmed confirmed = CMSG_REMOTE_DEVICE_AUTHORIZATION_CONFIRMED__INIT;
    confirmed.result = result;
    IHS_ClientSend(client, address, k_ERemoteDeviceAuthorizationConfirmed, (ProtobufCMessage *)&confirmed);
}

bool IHS_ClientAuthorizationRequest(IHS_Client *client, const IHS_HostInfo *host, const char *pin) {
    IHS_AuthorizationState *state = calloc(1, sizeof(IHS_AuthorizationState));
    if (!state) return false;
    state->client = client;
    state->host = *host;
    strncpy(state->deviceName, client->base.deviceName, sizeof(state->deviceName) - 1);
    state->deviceName[sizeof(state->deviceName) - 1] = '\0';
    strncpy(state->pin, pin, sizeof(state->pin) - 1);
    state->pin[sizeof(state->pin) - 1] = '\0';
    state->requestId = IHS_CryptoRandomUInt32();
    if (!IHS_AuthorizationKeyExchangeInit(&state->keyExchange, state->pin)) {
        IHS_AuthorizationKeyExchangeClear(&state->keyExchange);
        free(state);
        return false;
    }
    if (!IHS_TimerTaskStartOwned(client->timers, &client->taskHandles.authorization,
            AuthorizationRequestTimer, AuthorizationRequestCleanup, 0, state)) {
        IHS_AuthorizationKeyExchangeClear(&state->keyExchange);
        free(state);
        return false;
    }
    return true;
}

static void AuthorizationCancelVisit(IHS_TimerTask *task, void *context) {
    IHS_Client *client = context;
    IHS_AuthorizationState *state = IHS_TimerTaskGetContext(task);
    CMsgRemoteDeviceAuthorizationCancelRequest request = CMSG_REMOTE_DEVICE_AUTHORIZATION_CANCEL_REQUEST__INIT;
    IHS_SocketAddress address = state->host.address;
    IHS_TimerTaskStopOwned(client->timers, &client->taskHandles.authorization);
    IHS_ClientSend(client, address, k_ERemoteDeviceAuthorizationCancelRequest, (ProtobufCMessage *) &request);
}

bool IHS_ClientAuthorizationCancel(IHS_Client *client) {
    return IHS_TimerTaskVisitOwned(client->timers, &client->taskHandles.authorization,
                                    AuthorizationCancelVisit, client);
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
    if (!IHS_ClientMatchesHost(&current->host, response->address, header)) return;
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
            AuthorizationConfirm(client, *response->address, k_ERemoteDeviceAuthorizationCanceled);
        return;
    }
    /* Official Android checks result before auth_key (protocol RE §128).
     * Failed key calculations are confirmed but keep the transaction alive. */
    if (resp->result == k_ERemoteDeviceAuthorizationSuccess && resp->has_auth_key) {
        AuthorizationKeyExchange(client, state, task, resp);
        return;
    }
    switch (resp->result) {
        case k_ERemoteDeviceAuthorizationInProgress:
            if (client->callbacks.authorization && client->callbacks.authorization->progress) {
                client->callbacks.authorization->progress(client, &state->host, client->callbackContexts.authorization);
            }
            return;
        case k_ERemoteDeviceAuthorizationSuccess:
            state->terminal = true;
            IHS_TimerTaskStop(task);
            if (client->callbacks.authorization && client->callbacks.authorization->success) {
                client->callbacks.authorization->success(client, &state->host, resp->steamid,
                                                         client->callbackContexts.authorization);
            }
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
    if (!present && header->msg_type == k_ERemoteDeviceAuthorizationResponse && message &&
        ((CMsgRemoteDeviceAuthorizationResponse *)message)->has_auth_key)
        AuthorizationConfirm(client, *address, k_ERemoteDeviceAuthorizationCanceled);
}

static uint64_t AuthorizationRequestTimer(int runCount, void *data) {
    IHS_AuthorizationState *state = data;
    IHS_Client *client = state->client;
    int64_t now = (int64_t)time(NULL);
    /* Original manager Update compares whole wall seconds, including clock
     * changes. The existing 1ms timer worker supplies our update cadence. */
    if (runCount && now < state->nextSendWallSecond) return 1;
    uint8_t pubKey[384];
    size_t pubKeyLen = sizeof(pubKey);
    if (!IHS_ClientAuthorizationPubKey(client, state->host.universe, pubKey, &pubKeyLen)) {
        state->terminal = true;
        state->timerFailed = true;
        AuthorizationSendCancel(client, &state->host);
        return 0;
    }
    uint8_t secret[32], token[32];
    IHS_BaseGetSecretKey(&client->base, secret);
    ProtobufCBinaryData deviceToken = {.data = token, .len = sizeof(token)};

    /* Initialize and serialize ticket */
    CMsgRemoteDeviceAuthorizationRequest__CKeyEscrowTicket ticket =
            CMSG_REMOTE_DEVICE_AUTHORIZATION_REQUEST__CKEY_ESCROW__TICKET__INIT;
    AuthorizationConfigureTicket(client, state, secret, &ticket);
    uint8_t serTicket[2048];
    size_t serTicketLen = protobuf_c_message_pack(&ticket.base, serTicket);

    /* RSA encrypt ticket data */
    uint8_t encryptedTicket[2048];
    ProtobufCBinaryData encryptedRequest = {.data = encryptedTicket, .len = sizeof(encryptedTicket)};
    if (IHS_CryptoRSAEncrypt(serTicket, serTicketLen, pubKey, pubKeyLen, encryptedRequest.data,
                              &encryptedRequest.len) != 0) {
        memset(secret, 0, sizeof(secret));
        state->terminal = true;
        state->timerFailed = true;
        AuthorizationSendCancel(client, &state->host);
        return 0;
    }

    bool tokenReady = IHS_ClientCreateDeviceToken(client, secret, token);
    memset(secret, 0, sizeof(secret));
    if (!tokenReady) {
        state->terminal = true;
        state->timerFailed = true;
        AuthorizationSendCancel(client, &state->host);
        return 0;
    }

    uint8_t authKey[IHS_CRYPTO_X25519_KEY_SIZE];
    IHS_AuthorizationKeyExchangeMaskedKey(&state->keyExchange, authKey);

    CMsgRemoteDeviceAuthorizationRequest request = CMSG_REMOTE_DEVICE_AUTHORIZATION_REQUEST__INIT;
    request.device_name = state->deviceName;
    request.device_token = deviceToken;
    request.encrypted_request = encryptedRequest;
    request.has_auth_key = true;
    request.auth_key.data = authKey;
    request.auth_key.len = sizeof(authKey);
    request.has_request_id = true;
    request.request_id = state->requestId;

    IHS_SocketAddress address = state->host.address;
    IHS_ClientSend(client, address, k_ERemoteDeviceAuthorizationRequest, (ProtobufCMessage *) &request);
    int64_t sentAt = (int64_t)time(NULL);
    state->nextSendWallSecond = (int64_t)((uint64_t)sentAt + 3);
    return 1;
}

static void AuthorizationConfigureTicket(IHS_Client *client, IHS_AuthorizationState *state,
                                         const uint8_t secret[32],
                                         CMsgRemoteDeviceAuthorizationRequest__CKeyEscrowTicket *ticket) {
    ticket->has_password = true;
    ticket->password.len = strlen(state->pin);
    ticket->password.data = (uint8_t *) state->pin;

    ticket->has_identifier = true;
    ticket->identifier = client->base.deviceId;

    ticket->has_payload = true;
    ticket->payload.len = sizeof(client->base.secretKey);
    ticket->payload.data = (uint8_t *)secret;

    ticket->has_usage = true;
    ticket->usage = k_EKeyEscrowUsageStreamingDevice;

    ticket->device_name = client->base.deviceName;
}

static void AuthorizationKeyExchange(IHS_Client *client, IHS_AuthorizationState *state,
                                     IHS_TimerTask *task,
                                     const CMsgRemoteDeviceAuthorizationResponse *resp) {
    IHS_AuthorizationCache *cache = &client->authorizationCache;
    size_t count = resp->auth_key.len;
    IHS_BaseLock(&client->base);
    bool cached = count == cache->keyLen &&
                  (!count || !memcmp(resp->auth_key.data, cache->key, count));
    ERemoteDeviceAuthorizationResult cachedResult = cache->result;
    IHS_BaseUnlock(&client->base);
    if (cached) {
        AuthorizationConfirm(client, state->host.address, cachedResult);
        return; /* Cached success confirms again but never advances twice. */
    }
    uint8_t *key = count ? malloc(count) : NULL;
    if (count && !key) {
        /* Local allocation failure guard, not an inferred protocol result. */
        AuthorizationFail(client, state, task, IHS_AuthorizationFailed);
        return;
    }
    if (count) memcpy(key, resp->auth_key.data, count);
    uint8_t secretKey[IHS_CRYPTO_SHA256_SIZE] = {0};
    bool exhausted = state->keyExchangeAttempts++ >= 3;
    bool ok = !exhausted && resp->has_device_token &&
              IHS_AuthorizationKeyExchangeComplete(&state->keyExchange, resp->auth_key.data,
                                                   count, resp->device_token.data,
                                                   resp->device_token.len, client->base.deviceId,
                                                   secretKey);
    IHS_BaseLock(&client->base);
    free(cache->key);
    cache->key = key;
    cache->keyLen = count;
    cache->result = ok ? k_ERemoteDeviceAuthorizationSuccess : k_ERemoteDeviceAuthorizationDenied;
    IHS_BaseUnlock(&client->base);
    if (exhausted) {
        state->terminal = true;
        IHS_TimerTaskStop(task);
        AuthorizationSendCancel(client, &state->host);
        AuthorizationConfirm(client, state->host.address, cache->result);
        if (client->callbacks.authorization && client->callbacks.authorization->failed)
            client->callbacks.authorization->failed(client, &state->host, IHS_AuthorizationDenied,
                                                    client->callbackContexts.authorization);
        return;
    }
    if (ok) {
        IHS_BaseSetSecretKey(&client->base, secretKey);
        if (client->callbacks.authorization && client->callbacks.authorization->persist)
            client->callbacks.authorization->persist(client, &state->host, resp->steamid,
                secretKey, client->callbackContexts.authorization);
    }
    memset(secretKey, 0, sizeof(secretKey));
    AuthorizationConfirm(client, state->host.address, cache->result);
    if (!ok)
        return;
    state->terminal = true;
    IHS_TimerTaskStop(task);
    if (client->callbacks.authorization && client->callbacks.authorization->success)
        client->callbacks.authorization->success(client, &state->host, resp->steamid,
                                                 client->callbackContexts.authorization);
}

static void AuthorizationRequestCleanup(void *data) {
    IHS_AuthorizationState *state = data;
    IHS_AuthorizationKeyExchangeClear(&state->keyExchange);
    /* Most-recent public auth_key/result outlive this private request material. */
    if (state->timerFailed && state->client->callbacks.authorization &&
        state->client->callbacks.authorization->failed)
        state->client->callbacks.authorization->failed(state->client, &state->host,
            IHS_AuthorizationFailed, state->client->callbackContexts.authorization);
    free(data);
}
