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
#include "protobuf/pb_utils.h"

#include <stdlib.h>
#include <string.h>

typedef struct DiscoveryContext {
    IHS_Client *client;
    uint32_t interval;
} DiscoveryContext;
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
    *context = (DiscoveryContext){client, interval};
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

void IHS_ClientDiscoveryCallback(IHS_Client *client, const IHS_SocketAddress *address,
                                 CMsgRemoteClientBroadcastHeader *header,
                                 ProtobufCMessage *message) {
    if (header->msg_type == k_ERemoteClientBroadcastMsgStatus) {
        CMsgRemoteClientBroadcastStatus *status = (CMsgRemoteClientBroadcastStatus *)message;
        IHS_HostInfo info;
        info.clientId = header->client_id;
        info.instanceId = header->instance_id;
        info.address = *address;
        info.ostype = status->ostype;
        info.universe = status->euniverse;
        info.gamesRunning = status->games_running;
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
    IHS_ClientLog(client, IHS_LogLevelVerbose, "Discovery", "Send broadcast");
    DiscoveryBroadcast(client);
    return context->interval;
}

static bool DiscoveryBroadcast(IHS_Client *client) {
    CMsgRemoteClientBroadcastDiscovery discovery = CMSG_REMOTE_CLIENT_BROADCAST_DISCOVERY__INIT;
    PROTOBUF_C_SET_VALUE(discovery, seq_num, client->discoverySeq++);

    return IHS_ClientBroadcast(client, k_ERemoteClientBroadcastMsgDiscovery,
                               (ProtobufCMessage *)&discovery);
}