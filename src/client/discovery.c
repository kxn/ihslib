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

#include "discovery_pri.h"
#include "protobuf/pb_utils.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <limits.h>

typedef IHS_DiscoveryContext DiscoveryContext;
static uint64_t DiscoveryTimerRun(int runCount, DiscoveryContext *context);
static bool DiscoveryBroadcast(IHS_Client *client);
static void DiscoveryPresent(IHS_TimerTask *task, void *context) {
    (void)task;
    *(bool *)context = true;
}
bool IHS_ClientStartDiscovery(IHS_Client *client, uint32_t interval) {
    DiscoveryContext *context = malloc(sizeof(*context));
    if (!context)
        return false;
    *context = (DiscoveryContext){.client = client, .interval = interval};
    if (!IHS_TimerTaskStartOwned(client->timers, &client->discoveryTimer,
                                 (IHS_TimerRunFunction *)DiscoveryTimerRun, free, 0, context)) {
        free(context);
        return false;
    }
    return true;
}
bool IHS_ClientStopDiscovery(IHS_Client *client) {
    bool present = false;
    IHS_TimerTaskVisitOwned(client->timers, &client->discoveryTimer, DiscoveryPresent, &present);
    IHS_TimerTaskStopOwned(client->timers, &client->discoveryTimer);
    return present;
}

/* Visitors serialize all subscription changes with the task, then take Base:
 * never acquire the timer owner while holding Base. */
typedef struct DiscoveryChange { IHS_Client *client; int action; bool accepted; } DiscoveryChange;
static void ChangeDiscovery(IHS_TimerTask *task, void *opaque) {
    DiscoveryChange *change = opaque;
    DiscoveryContext *context = IHS_TimerTaskGetContext(task);
    if (!context->rounds) return;
    IHS_BaseLock(&change->client->base);
    if (!change->client->base.interrupted) {
        if (change->action == 1 && context->broadcastOwners < UINT_MAX) {
            ++context->broadcastOwners;
            context->full = 0;
            change->accepted = true;
        } else if (change->action == -1 && context->broadcastOwners) {
            --context->broadcastOwners;
            change->accepted = true;
        } else if (!change->action) {
            context->full = 0;
            change->accepted = true;
        }
    }
    IHS_BaseUnlock(&change->client->base);
}

static bool ActivateDiscovery(IHS_Client *client, int action) {
    DiscoveryChange change = {.client = client, .action = action};
    if (IHS_TimerTaskVisitOwned(client->timers, &client->discoveryTimer, ChangeDiscovery, &change))
        return change.accepted;
    IHS_BaseLock(&client->base);
    bool stopped = client->base.interrupted;
    IHS_BaseUnlock(&client->base);
    if (stopped) return false;
    DiscoveryContext *context = calloc(1, sizeof(*context));
    if (!context) return false;
    context->client = client;
    context->rounds = true;
    context->broadcastOwners = action == 1;
    IHS_DiscoveryRandomSeed(&context->random, (int32_t)((uint32_t)time(NULL) ^ (uint32_t)getpid()));
    if (IHS_TimerTaskStartOwned(client->timers, &client->discoveryTimer,
            (IHS_TimerRunFunction *)DiscoveryTimerRun, free, 0, context)) return true;
    free(context);
    /* A concurrent Enable may have published the manager first. */
    IHS_TimerTaskVisitOwned(client->timers, &client->discoveryTimer, ChangeDiscovery, &change);
    return change.accepted;
}

bool IHS_ClientEnableDiscovery(IHS_Client *client) {
    return ActivateDiscovery(client, 1);
}

bool IHS_ClientDisableDiscovery(IHS_Client *client) {
    DiscoveryChange change = {.client = client, .action = -1};
    IHS_TimerTaskVisitOwned(client->timers, &client->discoveryTimer, ChangeDiscovery, &change);
    return change.accepted;
}

bool IHS_ClientTriggerDiscovery(IHS_Client *client) {
    return ActivateDiscovery(client, 0);
}

static bool SameTarget(const IHS_DiscoveryTarget *a, const IHS_DiscoveryTarget *b) {
    if (a->clientId != b->clientId || a->ip.family != b->ip.family) return false;
    if (a->ip.family == IHS_IPAddressFamilyIPv4)
        return !memcmp(a->ip.v4.data, b->ip.v4.data, 4);
    return !memcmp(a->ip.v6.data, b->ip.v6.data, 16);
}

bool IHS_ClientSetDiscoveryTargets(IHS_Client *client,
        const IHS_DiscoveryTarget *targets, size_t count) {
    if ((count && !targets) || count > SIZE_MAX / (sizeof(*targets) + sizeof(uint64_t))) return false;
    for (size_t i = 0; i < count; ++i)
        if (targets[i].ip.family != IHS_IPAddressFamilyIPv4 &&
            targets[i].ip.family != IHS_IPAddressFamilyIPv6) return false;
    IHS_BaseLock(&client->base);
    bool same = count == client->discoveryTargetCount;
    for (size_t i = 0; same && i < count; ++i) same = SameTarget(&targets[i], &client->discoveryTargets[i]);
    if (same) { IHS_BaseUnlock(&client->base); return true; }
    IHS_DiscoveryTarget *copy = count ? malloc(count * (sizeof(*copy) + sizeof(uint64_t))) : NULL;
    if (count && !copy) { IHS_BaseUnlock(&client->base); return false; }
    uint64_t *ids = count ? (uint64_t *)(copy + count) : NULL;
    size_t idCount = 0;
    for (size_t i = 0; i < count; ++i) {
        copy[i] = targets[i];
        uint64_t id = targets[i].clientId;
        bool duplicate = !id;
        for (size_t j = 0; j < idCount; ++j) if (ids[j] == id) duplicate = true;
        if (!duplicate) ids[idCount++] = id;
    }
    free(client->discoveryTargets);
    client->discoveryTargets = copy;
    client->discoveryTargetCount = count;
    client->discoveryClientIds = ids;
    client->discoveryClientIdCount = idCount;
    client->discoveryTargetsChanged = true;
    IHS_BaseUnlock(&client->base);
    /* The same Base-protected publication requests a full round. The task
     * consumes it before choosing full/fast, after the complete RX batch. */
    return true;
}

static void RequestStatus(DiscoveryContext *context) {
    IHS_Client *client = context->client;
    CMsgRemoteClientBroadcastDiscovery discovery = CMSG_REMOTE_CLIENT_BROADCAST_DISCOVERY__INIT;
    PROTOBUF_C_SET_VALUE(discovery, seq_num, client->discoverySeq);
    for (size_t i = 0; i < client->discoveryTargetCount; ++i) {
        IHS_SocketAddress address = {.ip = client->discoveryTargets[i].ip, .port = 27036};
        IHS_ClientSend(client, address, k_ERemoteClientBroadcastMsgDiscovery, (ProtobufCMessage *)&discovery);
    }
    if (!context->broadcastOwners) {
        discovery.n_client_ids = client->discoveryClientIdCount;
        discovery.client_ids = client->discoveryClientIds;
    }
    if (context->broadcastOwners || discovery.n_client_ids)
        IHS_ClientBroadcast(client, k_ERemoteClientBroadcastMsgDiscovery, (ProtobufCMessage *)&discovery);
}

void IHS_ClientDiscoveryCallback(IHS_Client *client, const IHS_SocketAddress *address,
                                 CMsgRemoteClientBroadcastHeader *header,
                                 ProtobufCMessage *message) {
    if (header->msg_type == k_ERemoteClientBroadcastMsgStatus) {
        CMsgRemoteClientBroadcastStatus *status = (CMsgRemoteClientBroadcastStatus *)message;
        IHS_HostInfo info = {0};
        info.clientId = header->client_id;
        info.instanceId = header->instance_id;
        info.address = *address;
        info.ostype = status->ostype;
        info.universe = status->euniverse;
        info.gamesRunning = status->games_running;
        info.hasGamesRunning = status->has_games_running;
        info.hasTimestamp = status->has_timestamp;
        info.timestamp = status->timestamp;
        strncpy(info.hostname, status->hostname, sizeof(info.hostname) - 1);
        info.hostname[sizeof(info.hostname) - 1] = '\0';
        IHS_BaseLock(&client->base);
        const IHS_ClientDiscoveryCallbacks *callbacks = client->callbacks.discovery;
        void *context = client->callbackContexts.discovery;
        IHS_BaseUnlock(&client->base);
        if (callbacks && callbacks->discovered)
            callbacks->discovered(client, &info, context);
    }
}

static uint64_t DiscoveryTimerRun(int runCount, DiscoveryContext *context) {
    (void)runCount;
    IHS_Client *client = context->client;
    /* Timer visitor -> Base, matching the streaming due-send lock order.
     * Full reads and deferred callbacks must finish before an automatic
     * broadcast can replace the discovery descriptor. */
    IHS_BaseLock(&client->base);
    if (client->base.interrupted) {
        IHS_BaseUnlock(&client->base);
        return 0;
    }
    bool ready = !client->receiveBatchActive && !client->authorizationCount &&
                 !client->authorizationDispatching;
    bool fresh = context->receiveGeneration != client->receiveGeneration;
    bool done = false;
    if (ready && fresh) {
        context->receiveGeneration = client->receiveGeneration;
        if (context->rounds) {
            if (client->discoveryTargetsChanged) {
                context->full = 0;
                client->discoveryTargetsChanged = false;
            }
            int64_t now = (int64_t)time(NULL);
            bool send = false;
            if (now >= context->full) {
                context->full = now + 12 + IHS_DiscoveryRandomInt(&context->random, 1, 5);
                context->fast = now + IHS_DiscoveryRandomInt(&context->random, 1, 3);
                context->tries = 1;
                ++client->discoverySeq;
                send = true;
            } else if (context->tries <= 2 && now >= context->fast) {
                ++context->tries;
                context->fast = now + IHS_DiscoveryRandomInt(&context->random, 4, 7);
                send = true;
            }
            if (send) RequestStatus(context);
        } else {
            uint64_t now = IHS_TimerNow();
            if (!context->hasSent || now >= context->nextSend) {
                DiscoveryBroadcast(client);
                context->hasSent = true;
                context->nextSend = now + context->interval;
                done = context->interval == 0;
            }
        }
    }
    IHS_BaseUnlock(&client->base);
    /* Official watcher scheduling is 100ms; interval=0 remains a one-shot.
     * Wall seconds decide rounds; monotonic time only drives the watcher. */
    return done ? 0 : 100;
}

static bool DiscoveryBroadcast(IHS_Client *client) {
    CMsgRemoteClientBroadcastDiscovery discovery = CMSG_REMOTE_CLIENT_BROADCAST_DISCOVERY__INIT;
    PROTOBUF_C_SET_VALUE(discovery, seq_num, client->discoverySeq++);

    return IHS_ClientBroadcast(client, k_ERemoteClientBroadcastMsgDiscovery,
                               (ProtobufCMessage *)&discovery);
}
