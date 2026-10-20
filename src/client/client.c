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

#include "ihslib/client.h"

#include <memory.h>
#include <stdint.h>
#include <stdlib.h>

#include "base.h"
#include "client_pri.h"
#include "crypto.h"
#include "endianness.h"
#include "ihs_buffer.h"
#include "ihs_buffer_ext.h"
#include "ihs_udp.h"
#include "protobuf/discovery.pb-c.h"
#include "protobuf/pb_utils.h"

static void ClientInitialized(IHS_Base *base, void *context);
static bool ClientReceiveCycle(IHS_Base *base, void *context, IHS_UDPPacket *packet);

static const unsigned char PACKET_MAGIC[8] = {0xff, 0xff, 0xff, 0xff, 0x21, 0x4c, 0x5f, 0xa0};

static void ClientRecvCallback(IHS_Base *base, const IHS_SocketAddress *address, IHS_Buffer *data);

static const ProtobufCMessageDescriptor
    *MessageDescriptors[k_ERemoteClientBroadcastMsgPairingExclusivity + 1] = {
        &cmsg_remote_client_broadcast_discovery__descriptor,
        &cmsg_remote_client_broadcast_status__descriptor,
        NULL,
        &cmsg_remote_device_authorization_request__descriptor,
        &cmsg_remote_device_authorization_response__descriptor,
        &cmsg_remote_device_streaming_request__descriptor,
        &cmsg_remote_device_streaming_response__descriptor,
        &cmsg_remote_device_proof_request__descriptor,
        &cmsg_remote_device_proof_response__descriptor,
        &cmsg_remote_device_authorization_cancel_request__descriptor,
        &cmsg_remote_device_streaming_cancel_request__descriptor,
        &cmsg_remote_client_broadcast_client_iddeconflict__descriptor,
        &cmsg_remote_device_stream_transport_signal__descriptor,
        &cmsg_remote_device_streaming_progress__descriptor,
        &cmsg_remote_device_authorization_confirmed__descriptor,
        &cmsg_remote_client_broadcast_client_pairing_state__descriptor,
        &cmsg_remote_client_broadcast_client_pairing_exclusivity__descriptor,
};

static IHS_BaseRunCallbacks ClientRunCallbacks = {
    .initialized = ClientInitialized,
    .receiveCycle = ClientReceiveCycle,
    .receiveCapacity = 2048,
};

_Thread_local IHS_Client *IHS_ClientDispatchOwner;

IHS_Client *IHS_ClientCreate(const IHS_ClientConfig *config) {
    IHS_Client *client = malloc(sizeof(IHS_Client));
    if (!client) return NULL;
    memset(client, 0, sizeof(IHS_Client));
    client->authorizationCache.result = k_ERemoteDeviceAuthorizationFailed;
    /* BStart's initial no-subscriber Update has advanced the discovery round. */
    client->discoverySeq = 1;
    atomic_init(&client->deferAuthorization, false);
    IHS_BaseInit(&client->base, config, ClientRecvCallback, true);
    if (!client->base.lock) {
        free(client);
        return NULL;
    }
    IHS_BaseSetRunCallbacks(&client->base, &ClientRunCallbacks, NULL);
    client->receiveWake = IHS_CondCreate();
    if (!client->receiveWake) {
        IHS_BaseDestroy(&client->base);
        free(client);
        return NULL;
    }
    client->timers = IHS_TimerCreate();
    if (!client->timers) {
        IHS_CondDestroy(client->receiveWake);
        IHS_BaseDestroy(&client->base);
        free(client);
        return NULL;
    }

    client->privCallbacks.discovery = IHS_ClientDiscoveryCallback;
    client->privCallbacks.authorization = IHS_ClientAuthorizationCallback;
    client->privCallbacks.streaming = IHS_ClientStreamingCallback;

    if (!IHS_BaseStartWorker(&client->base, "IHSClient")) {
        IHS_TimerDestroy(client->timers);
        IHS_CondDestroy(client->receiveWake);
        IHS_BaseDestroy(&client->base);
        free(client);
        return NULL;
    }
    return client;
}

void IHS_ClientSetLogFunction(IHS_Client *client, IHS_LogFunction *logFunction) {
    IHS_BaseSetLogFunction(&client->base, logFunction);
}

void IHS_ClientGetSecretKey(IHS_Client *client, uint8_t *secretKey) {
    IHS_BaseGetSecretKey(&client->base, secretKey);
}

void IHS_ClientSetSecretKey(IHS_Client *client, const uint8_t *secretKey) {
    IHS_BaseSetSecretKey(&client->base, secretKey);
}

void IHS_ClientStop(IHS_Client *client) {
    IHS_BaseInterruptWorker(&client->base);
    IHS_BaseLock(&client->base);
    IHS_CondSignal(client->receiveWake);
    IHS_BaseUnlock(&client->base);
}

void IHS_ClientThreadedJoin(IHS_Client *client) {
    IHS_BaseWaitWorker(&client->base);
}

static void FreeAuthorizationFrame(CMsgRemoteClientBroadcastHeader *header, ProtobufCMessage *message) {
    cmsg_remote_client_broadcast_header__free_unpacked(header, NULL);
    if (message) protobuf_c_message_free_unpacked(message, NULL);
}

void IHS_ClientSetAuthorizationDeferred(IHS_Client *client, bool deferred) {
    atomic_store_explicit(&client->deferAuthorization, deferred, memory_order_release);
}

size_t IHS_ClientPumpAuthorization(IHS_Client *client, size_t budget) {
    if (!client) return 0;
    size_t dispatched = 0;
    while (dispatched < budget) {
        IHS_BaseLock(&client->base);
        if (!client->authorizationCount) {
            IHS_BaseUnlock(&client->base);
            break;
        }
        unsigned index = client->authorizationHead;
        IHS_SocketAddress address = client->authorizationQueue[index].address;
        CMsgRemoteClientBroadcastHeader *header = client->authorizationQueue[index].header;
        ProtobufCMessage *message = client->authorizationQueue[index].message;
        client->authorizationHead = (index + 1) % 32;
        --client->authorizationCount;
        ++client->authorizationDispatching;
        IHS_BaseUnlock(&client->base);
        /* Match BaseWorker's dispatch order: no Base lock around timer visitors. */
        IHS_Client *previousDispatch = IHS_ClientDispatchOwner;
        IHS_ClientDispatchOwner = client;
        if (header->msg_type == k_ERemoteDeviceAuthorizationResponse ||
            header->msg_type == k_ERemoteDeviceAuthorizationConfirmed ||
            header->msg_type == k_ERemoteDeviceAuthorizationRequest ||
            header->msg_type == k_ERemoteDeviceAuthorizationCancelRequest)
            client->privCallbacks.authorization(client, &address, header, message);
        else
            client->privCallbacks.streaming(client, &address, header, message);
        IHS_ClientDispatchOwner = previousDispatch;
        FreeAuthorizationFrame(header, message);
        IHS_BaseLock(&client->base);
        --client->authorizationDispatching;
        IHS_BaseUnlock(&client->base);
        ++dispatched;
    }
    IHS_BaseLock(&client->base);
    bool drained = client->authorizationCount == 0;
    IHS_BaseUnlock(&client->base);
    /* Original Update processes inbound packets before due sends. A bounded
     * pump which leaves queued work must not retry an already-obsolete phase. */
    if (budget && drained && atomic_load_explicit(&client->deferAuthorization, memory_order_acquire))
        IHS_ClientStreamingUpdate(client);
    if (budget && drained) IHS_ClientDiscoveryEndCycle(client);
    return dispatched;
}

void IHS_ClientDestroy(IHS_Client *client) {
    /* The caller has joined RX; queued callbacks must not outlive their client. */
    for (unsigned n = 0; n < client->authorizationCount; ++n) {
        unsigned index = (client->authorizationHead + n) % 32;
        FreeAuthorizationFrame(client->authorizationQueue[index].header,
                               client->authorizationQueue[index].message);
    }
    IHS_TimerDestroy(client->timers);
    free(client->discoveryTargets);
    free(client->authorizationCache.key);
    IHS_ClientLog(client, IHS_LogLevelInfo, "Client", "Destroying client, bye!");
    IHS_CondDestroy(client->receiveWake);
    IHS_BaseDestroy(&client->base);
    free(client);
}

void IHS_ClientSetDiscoveryCallbacks(IHS_Client *client,
                                     const IHS_ClientDiscoveryCallbacks *callbacks, void *context) {
    IHS_BaseLock(&client->base);
    client->callbacks.discovery = callbacks;
    client->callbackContexts.discovery = context;
    IHS_BaseUnlock(&client->base);
}

void IHS_ClientSetAuthorizationCallbacks(IHS_Client *client,
                                         const IHS_ClientAuthorizationCallbacks *callbacks,
                                         void *context) {
    client->callbacks.authorization = callbacks;
    client->callbackContexts.authorization = context;
}

void IHS_ClientSetStreamingCallbacks(IHS_Client *client,
                                     const IHS_ClientStreamingCallbacks *callbacks, void *context) {
    client->callbacks.streaming = callbacks;
    client->callbackContexts.streaming = context;
}

const char *IHS_ClientError(IHS_Client *client) {
    return "";
}

bool IHS_ClientCreateDeviceToken(IHS_Client *client, const uint8_t secret[32], uint8_t token[32]) {
    uint8_t id[8];
    IHS_WriteUInt64LE(id, client->base.deviceId);
    size_t count = 32;
    int result = IHS_CryptoSymmetricEncrypt(id, sizeof(id), secret, 32, token, &count);
    if (result != 0 || count != 32) {
        memset(token, 0, 32);
        return false;
    }
    return true;
}

bool IHS_ClientSend(IHS_Client *client, IHS_SocketAddress address, ERemoteClientBroadcastMsg type,
                    ProtobufCMessage *message) {
    CMsgRemoteClientBroadcastHeader header;
    cmsg_remote_client_broadcast_header__init(&header);
    header.has_client_id = 1;
    header.client_id = client->base.deviceId;
    header.has_msg_type = 1;
    header.msg_type = type;
    size_t header_size = cmsg_remote_client_broadcast_header__get_packed_size(&header);

    IHS_Buffer buf;
    IHS_BufferInit(&buf, 1024, 2048);
    IHS_BufferAppendMem(&buf, PACKET_MAGIC, sizeof(PACKET_MAGIC));
    IHS_BufferAppendUInt32LE(&buf, header_size);
    cmsg_remote_client_broadcast_header__pack(&header,
                                              IHS_BufferPointerForAppend(&buf, header_size));
    buf.size += header_size;
    if (message != NULL) {
        size_t payload_size = protobuf_c_message_get_packed_size(message);
        IHS_BufferAppendUInt32LE(&buf, payload_size);
        protobuf_c_message_pack(message, IHS_BufferPointerForAppend(&buf, payload_size));
        buf.size += payload_size;
    } else {
        IHS_BufferAppendUInt32LE(&buf, 0);
    }

    bool ret = IHS_BaseSend(&client->base, address, &buf);
    IHS_BufferClear(&buf, true);
    return ret;
}

bool IHS_ClientBroadcast(IHS_Client *client, ERemoteClientBroadcastMsg type,
                         ProtobufCMessage *message) {
    IHS_SocketAddress targets[9] = {{
        .ip = {.v4 = {IHS_IPAddressFamilyIPv4, {0xFF, 0xFF, 0xFF, 0xFF}}}, .port = 27036}};
    /* 255.255.255.255 alone doesn't reach Steam hosts from every device and
     * network; send to each local subnet's broadcast address as well. Hosts
     * answer every copy, discovery already handles repeated status. */
    IHS_IPAddress directed[8];
    size_t count = IHS_UDPBroadcastAddresses(directed, sizeof(directed) / sizeof(directed[0]));
    for (size_t i = 0; i < count; i++) {
        targets[i + 1] = (IHS_SocketAddress){.ip = directed[i], .port = 27036};
    }
    /* Original CDiscoverySocket::BSend fixes its destination list, then retries
     * the whole list after a failed pass, at most twice. Only the raw socket
     * changes; queued transactions and the client/RX owner remain intact. */
    IHS_BaseLock(&client->base);
    bool sent = false;
    for (unsigned pass = 0; pass < 3 && !client->base.interrupted && client->base.socket; ++pass) {
        for (size_t i = 0; i <= count; ++i)
            sent = IHS_ClientSend(client, targets[i], type, message) || sent;
        if (sent) break;
        if (pass < 2) IHS_UDPSocketReopen(client->base.socket);
    }
    IHS_BaseUnlock(&client->base);
    return sent;
}

static void ClientRecvCallback(IHS_Base *base, const IHS_SocketAddress *address, IHS_Buffer *data) {
    // Any host on the LAN can send a UDP datagram here. Validate every byte before
    // dereferencing — the audit found four crash vectors at this entry point.
    // Minimum well-formed packet: PACKET_MAGIC + 4-byte header_size + 4-byte payload_size.
    const size_t MIN_DATAGRAM = sizeof(PACKET_MAGIC) + 4 + 4;
    if (data->size < MIN_DATAGRAM)
        return;
    if (memcmp(IHS_BufferPointer(data), PACKET_MAGIC, sizeof(PACKET_MAGIC)) != 0) {
        IHS_BaseLog(base, IHS_LogLevelDebug, "Client", "Unrecognized packet!");
        return;
    }
    IHS_BufferOffsetBy(data, sizeof(PACKET_MAGIC));

    uint32_t header_size, payload_size;
    IHS_BufferOffsetBy(data, (int)IHS_ReadUInt32LE(IHS_BufferPointer(data), &header_size));
    // header_size must fit in the remaining buffer along with the 4-byte payload_size prefix.
    if (header_size > data->size || data->size - header_size < 4)
        return;

    CMsgRemoteClientBroadcastHeader *header =
        IHS_UNPACK_BUFFER_SIZE(cmsg_remote_client_broadcast_header__unpack, data, header_size);
    if (header == NULL) {
        IHS_BaseLog(base, IHS_LogLevelDebug, "Client", "Malformed broadcast header");
        return;
    }
    /* Fixed official LAN BProcess rejects zero/default identity before dispatch. */
    if (!header->has_client_id || !header->client_id) {
        cmsg_remote_client_broadcast_header__free_unpacked(header, NULL);
        return;
    }
    IHS_BufferOffsetBy(data, (int)header_size);
    IHS_BufferOffsetBy(data, (int)IHS_ReadUInt32LE(IHS_BufferPointer(data), &payload_size));
    if (payload_size != data->size) {
        cmsg_remote_client_broadcast_header__free_unpacked(header, NULL);
        return;
    }

    ERemoteClientBroadcastMsg type = header->msg_type;
    // The MessageDescriptors table only covers entries explicitly known to this client.
    // Any enum value beyond that (or negative) would read OOB and dereference garbage.
    const ProtobufCMessageDescriptor *descriptor = NULL;
    if ((int)type >= 0 &&
        (size_t)type < sizeof(MessageDescriptors) / sizeof(MessageDescriptors[0])) {
        descriptor = MessageDescriptors[type];
    }
    ProtobufCMessage *message =
        descriptor
            ? protobuf_c_message_unpack(descriptor, NULL, payload_size, IHS_BufferPointer(data))
            : NULL;
    if (type == k_ERemoteDeviceProofRequest && !message) {
        cmsg_remote_client_broadcast_header__free_unpacked(header, NULL);
        return;
    }
    IHS_Client *client = (IHS_Client *)base;
    switch (type) {
    case k_ERemoteClientBroadcastMsgDiscovery:
    case k_ERemoteClientBroadcastMsgStatus:
    case k_ERemoteClientBroadcastMsgOffline:
    case k_ERemoteClientBroadcastMsgClientIDDeconflict:
        client->privCallbacks.discovery(client, address, header, message);
        break;
    case k_ERemoteDeviceAuthorizationRequest:
    case k_ERemoteDeviceAuthorizationResponse:
    case k_ERemoteDeviceAuthorizationCancelRequest:
    case k_ERemoteDeviceAuthorizationConfirmed:
    case k_ERemoteDeviceStreamingRequest:
    case k_ERemoteDeviceStreamingResponse:
    case k_ERemoteDeviceStreamingProgress:
    case k_ERemoteDeviceStreamingCancelRequest:
    case k_ERemoteDeviceProofRequest:
    case k_ERemoteDeviceProofResponse:
        if (atomic_load_explicit(&client->deferAuthorization, memory_order_acquire)) {
            IHS_BaseLock(base);
            if (client->authorizationCount < 32) {
                unsigned index = (client->authorizationHead + client->authorizationCount++) % 32;
                client->authorizationQueue[index].address = *address;
                client->authorizationQueue[index].header = header;
                client->authorizationQueue[index].message = message;
                IHS_BaseUnlock(base);
            } else {
                ++client->authorizationDropped;
                IHS_BaseUnlock(base);
                FreeAuthorizationFrame(header, message);
            }
            return; /* Queue owns header/message; a full queue drops and frees them. */
        }
        if (type == k_ERemoteDeviceAuthorizationResponse || type == k_ERemoteDeviceAuthorizationConfirmed ||
            type == k_ERemoteDeviceAuthorizationRequest || type == k_ERemoteDeviceAuthorizationCancelRequest)
            client->privCallbacks.authorization(client, address, header, message);
        else
            client->privCallbacks.streaming(client, address, header, message);
        break;
    case k_ERemoteClientBroadcastMsgPairingState:
    case k_ERemoteClientBroadcastMsgPairingExclusivity:
        IHS_BaseLog(base, IHS_LogLevelInfo, "Client",
                    "Unhandled pairing broadcast msg_type=%d payload_size=%u", (int)type,
                    payload_size);
        break;
    default:
        break;
    }
    cmsg_remote_client_broadcast_header__free_unpacked(header, NULL);
    // protobuf_c_message_free_unpacked must not be called on NULL — it crashes on at
    // least some protobuf-c versions when the underlying message had no allocations
    // beyond the descriptor lookup.
    if (message != NULL) {
        protobuf_c_message_free_unpacked(message, NULL);
    }
}

static void ClientInitialized(IHS_Base *base, void *context) {
    (void)context;
    /* Official CDiscoverySocket::BInit requests O_NONBLOCK (@caddbc).
     * CServerManagerNetworkWatcher schedules Update at 100ms (@c9643c). */
    IHS_UDPSocketSetBlocking(base->socket, false);
}

static bool ClientReceiveCycle(IHS_Base *base, void *context, IHS_UDPPacket *packet) {
    (void)context;
    IHS_Client *client = (IHS_Client *)base;
    if (!IHS_ClientDiscoveryBeginCycle(client)) {
        IHS_BaseLock(base);
        if (!base->interrupted) IHS_CondWaitTimeout(client->receiveWake, base->lock, 100);
        bool running = !base->interrupted;
        IHS_BaseUnlock(base);
        return running;
    }
    IHS_BaseLock(base);
    if (base->interrupted) {
        IHS_BaseUnlock(base);
        return false;
    }
    client->receiveBatchActive = true;
    IHS_BaseUnlock(base);
    uint64_t due = IHS_TimerNow() + 100;
    while (!base->interrupted) {
        /* Discovery broadcast recovery can replace the raw fd. It holds Base
         * through the change; no receive may retain the old descriptor. */
        IHS_BaseLock(base);
        int result = IHS_UDPSocketReceive(base->socket, packet);
        IHS_BaseUnlock(base);
        /* Original BReceive @cae2c0 treats all nonpositive recvfrom results
         * alike: end this Update's batch, retain socket for the next Update.
         * This client owner differs from the media session's receiveCycle. */
        if (result <= 0) break;
        IHS_Client *previousDispatch = IHS_ClientDispatchOwner;
        IHS_ClientDispatchOwner = client;
        base->callbacks.received(base, &packet->address, &packet->buffer);
        IHS_ClientDispatchOwner = previousDispatch;
        IHS_BufferClear(&packet->buffer, false);
    }
    IHS_BufferClear(&packet->buffer, false);
    IHS_BaseLock(base);
    client->receiveBatchActive = false;
    ++client->receiveGeneration;
    bool update = !base->interrupted && !atomic_load(&client->deferAuthorization);
    IHS_BaseUnlock(base);
    /* Original manager Update drains input before checking due sends. Deferred
     * owners do this after draining their published callback queue instead. */
    if (update) IHS_ClientStreamingUpdate(client);
    IHS_ClientDiscoveryEndCycle(client);
    IHS_BaseLock(base);
    while (!base->interrupted) {
        uint64_t now = IHS_TimerNow();
        if (now >= due) break;
        IHS_CondWaitTimeout(client->receiveWake, base->lock, (uint32_t)(due - now));
    }
    bool running = !base->interrupted;
    IHS_BaseUnlock(base);
    return running;
}
