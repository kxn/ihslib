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

static void DestroyDiscovery(void *opaque) {
    DiscoveryContext *context = opaque;
    for (unsigned n = 0; n < context->commandCount; ++n) {
        unsigned index = (context->commandHead + n) % IHS_DISCOVERY_COMMAND_CAPACITY;
        if (context->commands[index].operation == IHS_DiscoveryTargets)
            free(context->commands[index].targets);
    }
    free(context);
}
static void DiscoveryPresent(IHS_TimerTask *task, void *context) {
    (void)task;
    *(bool *)context = true;
}
bool IHS_ClientStartDiscovery(IHS_Client *client, uint32_t interval) {
    DiscoveryContext *context = malloc(sizeof(*context));
    if (!context) return false;
    *context = (DiscoveryContext){.client = client, .interval = interval};
    if (!IHS_TimerTaskStartOwned(client->timers, &client->discoveryTimer,
            (IHS_TimerRunFunction *)DiscoveryTimerRun, DestroyDiscovery, 0, context)) {
        DestroyDiscovery(context);
        return false;
    }
    return true;
}
bool IHS_ClientStopDiscovery(IHS_Client *client) {
    bool present = false;
    IHS_TimerTaskVisitOwned(client->timers, &client->discoveryTimer, DiscoveryPresent, &present);
    IHS_TimerTaskStopOwned(client->timers, &client->discoveryTimer);
    IHS_BaseLock(&client->base);
    IHS_CondSignal(client->receiveWake);
    IHS_BaseUnlock(&client->base);
    return present;
}

static bool SameTargets(const IHS_DiscoveryTarget *a, size_t na,
                        const IHS_DiscoveryTarget *b, size_t nb) {
    if (na != nb) return false;
    for (size_t i = 0; i < na; ++i)
        if (a[i].clientId != b[i].clientId || IHS_IPAddressCompare(&a[i].ip, &b[i].ip))
            return false;
    return true;
}
static bool CopyTargets(const IHS_DiscoveryTarget *targets, size_t count,
                        IHS_DiscoveryCommand *command) {
    IHS_DiscoveryTarget *copy = count ? malloc(count * (sizeof(*copy) + sizeof(uint64_t))) : NULL;
    if (count && !copy) return false;
    uint64_t *ids = count ? (uint64_t *)(copy + count) : NULL;
    size_t idCount = 0;
    for (size_t i = 0; i < count; ++i) {
        copy[i] = targets[i];
        uint64_t id = targets[i].clientId;
        bool duplicate = !id;
        for (size_t j = 0; j < idCount; ++j) if (ids[j] == id) duplicate = true;
        if (!duplicate) ids[idCount++] = id;
    }
    *command = (IHS_DiscoveryCommand){.operation=IHS_DiscoveryTargets,
        .targets=copy, .count=count, .idCount=idCount};
    return true;
}
static void PublishTargets(IHS_Client *client, IHS_DiscoveryCommand *command) {
    free(client->discoveryTargets);
    client->discoveryTargets = command->targets;
    client->discoveryTargetCount = command->count;
    client->discoveryClientIds = command->count ? (uint64_t *)(command->targets + command->count) : NULL;
    client->discoveryClientIdCount = command->idCount;
    client->discoveryTargetsChanged = true;
    command->targets = NULL; /* Ownership transferred to the client. */
}
static void RefreshDesired(DiscoveryContext *context) {
    if (!context->queuedTargets) {
        context->desiredTargets = context->client->discoveryTargets;
        context->desiredCount = context->client->discoveryTargetCount;
    }
}
static void PushCommand(DiscoveryContext *context, IHS_DiscoveryCommand command) {
    unsigned index = (context->commandHead + context->commandCount) % IHS_DISCOVERY_COMMAND_CAPACITY;
    context->commands[index] = command;
    ++context->commandCount;
}
/* All callers hold timer -> Base, including initial publication. */
static void AdoptCycle(DiscoveryContext *context) {
    if (context->cycleActive) return;
    IHS_Client *client = context->client;
    context->cycleActive = true;
    context->receiveGeneration = client->receiveGeneration - (client->receiveBatchActive ? 0 : 1);
}
typedef struct DiscoveryChange {
    IHS_Client *client;
    IHS_DiscoveryOperation operation;
    const IHS_DiscoveryTarget *targets;
    size_t count;
    bool accepted;
} DiscoveryChange;
static void ChangeDiscovery(IHS_TimerTask *task, void *opaque) {
    DiscoveryChange *change = opaque;
    IHS_Client *client = change->client;
    DiscoveryContext *context = task ? IHS_TimerTaskGetContext(task) : NULL;
    IHS_BaseLock(&client->base);
    if (client->base.interrupted) goto done;
    bool rounds = context && context->rounds;
    bool nested = IHS_ClientDispatchOwner == client;
    if (change->operation == IHS_DiscoveryTargets) {
        const IHS_DiscoveryTarget *desired = rounds && !nested ? context->desiredTargets : client->discoveryTargets;
        size_t desiredCount = rounds && !nested ? context->desiredCount : client->discoveryTargetCount;
        if (SameTargets(change->targets, change->count, desired, desiredCount)) {
            change->accepted = true;
            goto done;
        }
        bool queue = rounds && !nested && (context->cycleActive || context->commandCount);
        if (queue && context->commandCount == IHS_DISCOVERY_COMMAND_CAPACITY) goto done;
        IHS_DiscoveryCommand command;
        if (!CopyTargets(change->targets, change->count, &command)) goto done;
        if (queue) {
            PushCommand(context, command);
            ++context->queuedTargets;
            context->desiredTargets = command.targets;
            context->desiredCount = command.count;
        } else {
            PublishTargets(client, &command);
            if (rounds) {
                context->full = 0;
                RefreshDesired(context);
                if (nested) AdoptCycle(context);
            }
        }
        change->accepted = true;
        goto done;
    }
    if (context && !rounds) goto done; /* Explicit-interval task is a separate API. */
    if (!context) {
        if (change->operation == IHS_DiscoveryDisable) goto done;
        context = calloc(1, sizeof(*context));
        if (!context) goto done;
        context->client = client;
        context->rounds = true;
        RefreshDesired(context);
        IHS_DiscoveryRandomSeed(&context->random, (int32_t)((uint32_t)time(NULL) ^ (uint32_t)getpid()));
        context->acceptedOwners = change->operation == IHS_DiscoveryEnable;
        if (nested) {
            AdoptCycle(context);
            context->broadcastOwners = context->acceptedOwners;
        } else PushCommand(context, (IHS_DiscoveryCommand){.operation=change->operation});
        if (!IHS_TimerTaskStartOwned(client->timers, &client->discoveryTimer,
                (IHS_TimerRunFunction *)DiscoveryTimerRun, DestroyDiscovery, 0, context)) {
            DestroyDiscovery(context);
            goto done;
        }
        change->accepted = true;
        goto done;
    }
    if (change->operation == IHS_DiscoveryEnable && context->acceptedOwners == UINT_MAX) goto done;
    if (change->operation == IHS_DiscoveryDisable && !context->acceptedOwners) goto done;
    if (nested) {
        if (change->operation == IHS_DiscoveryDisable && !context->broadcastOwners) goto done;
        if (change->operation == IHS_DiscoveryEnable) {
            ++context->broadcastOwners;
            ++context->acceptedOwners;
            context->full = 0;
        } else if (change->operation == IHS_DiscoveryDisable) {
            --context->broadcastOwners;
            --context->acceptedOwners;
        } else context->full = 0;
        AdoptCycle(context);
    } else if (change->operation == IHS_DiscoveryDisable && !context->cycleActive && !context->commandCount) {
        --context->broadcastOwners;
        --context->acceptedOwners;
    } else {
        if (context->commandCount == IHS_DISCOVERY_COMMAND_CAPACITY) goto done;
        PushCommand(context, (IHS_DiscoveryCommand){.operation=change->operation});
        if (change->operation == IHS_DiscoveryEnable) ++context->acceptedOwners;
        if (change->operation == IHS_DiscoveryDisable) --context->acceptedOwners;
    }
    change->accepted = true;
done:
    if (change->accepted) IHS_CondSignal(client->receiveWake);
    IHS_BaseUnlock(&client->base);
}
static bool SubmitDiscovery(IHS_Client *client, IHS_DiscoveryOperation operation) {
    DiscoveryChange change = {.client=client, .operation=operation};
    IHS_TimerTaskVisitSlot(client->timers, &client->discoveryTimer, ChangeDiscovery, &change);
    return change.accepted;
}
bool IHS_ClientEnableDiscovery(IHS_Client *client) { return SubmitDiscovery(client, IHS_DiscoveryEnable); }
bool IHS_ClientDisableDiscovery(IHS_Client *client) { return SubmitDiscovery(client, IHS_DiscoveryDisable); }
bool IHS_ClientTriggerDiscovery(IHS_Client *client) { return SubmitDiscovery(client, IHS_DiscoveryTrigger); }
bool IHS_ClientSetDiscoveryTargets(IHS_Client *client, const IHS_DiscoveryTarget *targets, size_t count) {
    if ((count && !targets) || count > SIZE_MAX / (sizeof(*targets) + sizeof(uint64_t))) return false;
    for (size_t i = 0; i < count; ++i)
        if (targets[i].ip.family != IHS_IPAddressFamilyIPv4 && targets[i].ip.family != IHS_IPAddressFamilyIPv6)
            return false;
    DiscoveryChange change = {.client=client, .operation=IHS_DiscoveryTargets, .targets=targets, .count=count};
    IHS_TimerTaskVisitSlot(client->timers, &client->discoveryTimer, ChangeDiscovery, &change);
    return change.accepted;
}

typedef struct DiscoveryBegin { IHS_Client *client; bool allowed; } DiscoveryBegin;
static void BeginDiscovery(IHS_TimerTask *task, void *opaque) {
    DiscoveryBegin *begin = opaque;
    IHS_Client *client = begin->client;
    DiscoveryContext *context = task ? IHS_TimerTaskGetContext(task) : NULL;
    IHS_BaseLock(&client->base);
    if (client->base.interrupted) goto done;
    if (context && context->rounds) {
        if (context->cycleActive) goto done;
        while (context->commandCount) {
            IHS_DiscoveryCommand command = context->commands[context->commandHead];
            context->commands[context->commandHead] = (IHS_DiscoveryCommand){0};
            context->commandHead = (context->commandHead + 1) % IHS_DISCOVERY_COMMAND_CAPACITY;
            --context->commandCount;
            if (command.operation == IHS_DiscoveryTargets) {
                --context->queuedTargets;
                PublishTargets(client, &command);
                context->full = 0;
                RefreshDesired(context);
            } else if (command.operation == IHS_DiscoveryDisable) {
                if (context->broadcastOwners) --context->broadcastOwners;
            } else {
                if (command.operation == IHS_DiscoveryEnable) ++context->broadcastOwners;
                context->full = 0;
                break; /* Its complete read/dispatch/send Update precedes later operations. */
            }
        }
        context->cycleActive = true;
        context->receiveGeneration = client->receiveGeneration;
    }
    client->receiveBatchActive = true;
    begin->allowed = true;
done:
    IHS_BaseUnlock(&client->base);
}
bool IHS_ClientDiscoveryBeginCycle(IHS_Client *client) {
    DiscoveryBegin begin = {.client=client};
    IHS_TimerTaskVisitSlot(client->timers, &client->discoveryTimer, BeginDiscovery, &begin);
    return begin.allowed;
}
static void EndDiscovery(IHS_TimerTask *task, void *opaque) {
    (void)opaque;
    DiscoveryContext *context = IHS_TimerTaskGetContext(task);
    if (context->rounds) (void)DiscoveryTimerRun(0, context);
}
void IHS_ClientDiscoveryEndCycle(IHS_Client *client) {
    IHS_TimerTaskVisitOwned(client->timers, &client->discoveryTimer, EndDiscovery, NULL);
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
    if (ready && fresh && (!context->rounds || context->cycleActive)) {
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
            context->cycleActive = false;
            IHS_CondSignal(client->receiveWake);
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
