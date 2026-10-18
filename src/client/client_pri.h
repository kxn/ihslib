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

#include "ihslib/client.h"

#include "protobuf/discovery.pb-c.h"
#include "base.h"
#include "ihs_timer.h"


typedef void (IHS_MessageCallback)(IHS_Client *client, const IHS_SocketAddress *address,
                                   CMsgRemoteClientBroadcastHeader *header, ProtobufCMessage *message);

/* One most-recent key/result for the manager lifetime, not per request or host.
 * Timer visitors serialize writers, Base protects inspection; free after timer join. */
typedef struct IHS_AuthorizationCache {
    uint8_t *key;
    size_t keyLen;
    ERemoteDeviceAuthorizationResult result;
} IHS_AuthorizationCache;

struct IHS_Client {
    IHS_Base base;
    struct {
        IHS_HostInfo host;
        uint32_t requestId;
    } canceled[8];
    unsigned canceledNext;
    IHS_HostInfo lastLaunchHost;
    uint32_t lastLaunchId;
    bool lastLaunchHasTicket; /* Successful reply retains explicit-cancel ownership. */
    IHS_AuthorizationCache authorizationCache;
    atomic_bool deferAuthorization;
    struct {
        IHS_SocketAddress address;
        CMsgRemoteClientBroadcastHeader *header;
        ProtobufCMessage *message;
    } authorizationQueue[32]; /* Local memory budget, not a Steam protocol limit. */
    unsigned authorizationHead, authorizationCount, authorizationDropped;
    IHS_Timer *timers;
    IHS_TimerTask *discoveryTimer;
    uint32_t discoveryInterval;
    uint32_t discoverySeq;
    struct {
        const IHS_ClientDiscoveryCallbacks *discovery;
        const IHS_ClientAuthorizationCallbacks *authorization;
        const IHS_ClientStreamingCallbacks *streaming;
    } callbacks;
    struct {
        void *discovery;
        void *authorization;
        void *streaming;
    } callbackContexts;
    struct {
        IHS_MessageCallback *discovery;
        IHS_MessageCallback *authorization;
        IHS_MessageCallback *streaming;
    } privCallbacks;
    struct {
        IHS_TimerTask *authorization;
        IHS_TimerTask *streaming;
    } taskHandles;
};

#define IHS_ClientLog(client, level, tag, ...) IHS_BaseLog((IHS_Base*) (client), (level), (tag), __VA_ARGS__)

bool IHS_ClientSend(IHS_Client *client, IHS_SocketAddress address, ERemoteClientBroadcastMsg type,
                    ProtobufCMessage *message);

/* Original BGetDeviceToken uses the producer's current secret and a fresh IV.
 * The caller supplies one snapshot shared with its authorization ticket. */
bool IHS_ClientCreateDeviceToken(IHS_Client *client, const uint8_t secret[32], uint8_t token[32]);

bool IHS_ClientBroadcast(IHS_Client *client, ERemoteClientBroadcastMsg type,
                         ProtobufCMessage *message);

bool IHS_ClientAuthorizationPubKey(IHS_Client *client, IHS_SteamUniverse universe, uint8_t *key, size_t *keyLen);

void IHS_ClientDiscoveryCallback(IHS_Client *client, const IHS_SocketAddress *address,
                                 CMsgRemoteClientBroadcastHeader *header, ProtobufCMessage *message);

void IHS_ClientAuthorizationCallback(IHS_Client *client, const IHS_SocketAddress *address,
                                     CMsgRemoteClientBroadcastHeader *header, ProtobufCMessage *message);

void IHS_ClientStreamingCallback(IHS_Client *client, const IHS_SocketAddress *address,
                                 CMsgRemoteClientBroadcastHeader *header, ProtobufCMessage *message);

/* Caller validates transaction IDs separately; status/identity cannot replace them. */
static inline bool IHS_ClientMatchesHost(const IHS_HostInfo *host,
        const IHS_SocketAddress *address, const CMsgRemoteClientBroadcastHeader *header) {
    return address->port == host->address.port &&
        IHS_IPAddressCompare(&address->ip, &host->address.ip) == 0 &&
        header->has_client_id && header->client_id == host->clientId &&
        (!host->instanceId || !header->has_instance_id || header->instance_id == host->instanceId);
}
