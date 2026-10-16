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

#include <stdbool.h>

#include "protobuf/remoteplay.pb-c.h"
#include "protobuf/pb_utils.h"

#include "session/session_pri.h"

#include "ch_discovery.h"
#include "ch_control.h"
#include "endianness.h"
#include "ihs_buffer_ext.h"
#include "hid/manager.h"

static void OnDiscoveryReceived(IHS_SessionChannel *channel, IHS_SessionPacket *packet);

static void OnConnectACK(IHS_SessionChannel *channel, const IHS_SessionPacket *packet);

static void OnUnconnected(IHS_SessionChannel *channel, const IHS_SessionPacket *packet);

static void OnPingRequest(IHS_SessionChannel *channel, const IHS_SessionPacket *packet,
                          const CDiscoveryPingRequest *request);

static const IHS_SessionChannelClass ChannelClass = {
        .received = OnDiscoveryReceived,
        .instanceSize = sizeof(IHS_SessionChannel)
};

IHS_SessionChannel *IHS_SessionChannelDiscoveryCreate(IHS_Session *session) {
    return IHS_SessionChannelCreate(&ChannelClass, session, IHS_SessionChannelTypeDiscovery,
                                    IHS_SessionChannelIdDiscovery, NULL);
}

void IHS_SessionChannelDiscoveryDisconnect(IHS_SessionChannel *channel) {
    /* Compatibility entry: application Close, no timer or repeated goodbye. */
    IHS_SessionDisconnect(channel->session);
}

static void OnDiscoveryReceived(IHS_SessionChannel *channel, IHS_SessionPacket *packet) {
    switch (packet->header.type) {
        case IHS_SessionPacketTypeConnectACK:
            OnConnectACK(channel, packet);
            break;
        case IHS_SessionPacketTypeUnconnected:
            OnUnconnected(channel, packet);
            break;
        case IHS_SessionPacketTypeDisconnect:
            IHS_SessionTransportDisconnect(channel->session);
            break;
        default:
            break;
    }
}

static void OnConnectACK(IHS_SessionChannel *channel, const IHS_SessionPacket *packet) {
    IHS_Session *session = channel->session;
    if (session->state.connectionId != packet->header.dstConnectionId) return;
    // Drop stale / replayed ACKs that arrive after the handshake has progressed.
    if (session->state.connectionState != IHS_SessionConnectionStateConnecting ||
        session->transport.timedOut || session->transport.detached ||
        IHS_SessionTransportState(session) != IHS_TransportConnecting) return;
    /* HandleConnected 0x7fbf18: exactly 13B header + 4B echoed time after CRC removal. */
    if (packet->body.size != 4) return;
    session->state.hostConnectionId = packet->header.srcConnectionId;
    session->state.crcRequired = packet->header.hasCrc;
    uint32_t echo;
    IHS_ReadUInt32LE(IHS_BufferPointer(&packet->body), &echo);
    IHS_StreamClockFeedback(&session->clock, echo, packet->header.sendTimestamp,
                            packet->header.receiveTimestamp);
    IHS_SessionChannel *control = IHS_SessionChannelFor(session, IHS_SessionChannelIdControl);
    IHS_SessionChannelControlHandshake(control, false);
}

static void OnUnconnected(IHS_SessionChannel *channel, const IHS_SessionPacket *packet) {
    // 1 byte type + 4 byte messageSize prefix is the minimum well-formed unconnected payload.
    // Without this guard, short LAN-attacker packets cause OOB reads through IHS_BufferPointerAt
    // and the messageSize field can also overflow the body bounds passed to protobuf-c.
    if (packet->body.size < 5) return;
    size_t offset = 0;
    EStreamDiscoveryMessage type = *IHS_BufferPointer(&packet->body);
    offset += 1;
    uint32_t messageSize;
    offset += IHS_ReadUInt32LE(IHS_BufferPointerAt(&packet->body, offset), &messageSize);
    if (offset + messageSize > packet->body.size) return;
    if (type == k_EStreamDiscoveryPingRequest) {
        CDiscoveryPingRequest *request = cdiscovery_ping_request__unpack(NULL, messageSize,
                                                                         IHS_BufferPointerAt(&packet->body, offset));
        if (request == NULL) return;
        OnPingRequest(channel, packet, request);
        cdiscovery_ping_request__free_unpacked(request, NULL);
    }
}

static void OnPingRequest(IHS_SessionChannel *channel, const IHS_SessionPacket *packet,
                          const CDiscoveryPingRequest *request) {
    CDiscoveryPingResponse response = CDISCOVERY_PING_RESPONSE__INIT;
    PROTOBUF_C_SET_VALUE(response, sequence, request->sequence);
    PROTOBUF_C_SET_VALUE(response, packet_size_received, IHS_PACKET_HEADER_SIZE + packet->body.size);
    size_t msgSize = cdiscovery_ping_response__get_packed_size(&response);

    IHS_SessionPacket outPacket;
    IHS_SessionChannelInitializePacket(channel, &outPacket, IHS_SessionPacketTypeUnconnected, true, 0);
    IHS_BufferAppendUInt8(&outPacket.body, k_EStreamDiscoveryPingResponse);
    IHS_BufferAppendUInt32LE(&outPacket.body, msgSize);
    IHS_BufferAppendMessage(&outPacket.body, (const ProtobufCMessage *) &response);

    IHS_SessionPacketPadTo(&outPacket, request->packet_size_requested);
    IHS_SessionQueuePacket(channel->session, &outPacket, false);
    IHS_SessionPacketClear(&outPacket, true);
}
