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

#include <stdio.h>

#include "ch_control.h"

#include "session/frame.h"
#include "session/session_pri.h"
#include "protobuf/pb_utils.h"

static void OnAuthenticationResponse(IHS_SessionChannel *channel, const CAuthenticationResponseMsg *message);

/* Original proto2 response parser (Linux 3207d0, protocol reference §108):
 * unknown enum values do not overwrite a prior known scalar. protobuf-c's
 * final enum values cannot recover that ordering. This message is consumed
 * locally; only its two scalar getters/presence bits need to be retained. */
static bool AuthVarint(const uint8_t *data, size_t size, size_t *offset, uint64_t *value) {
    *value = 0;
    for (unsigned n = 0; n < 10 && *offset < size; ++n) {
        uint8_t byte = data[(*offset)++];
        *value |= (uint64_t)(byte & 127u) << (7u * n);
        if (!(byte & 128u)) return true;
    }
    return false;
}

static bool AuthSkip(const uint8_t *data, size_t size, size_t *offset, uint32_t tag, unsigned depth) {
    uint64_t value;
    switch (tag & 7u) {
    case 0: return AuthVarint(data, size, offset, &value);
    case 1:
        if (size - *offset < 8) return false;
        *offset += 8;
        return true;
    case 2:
        if (!AuthVarint(data, size, offset, &value) || value > size - *offset) return false;
        *offset += (size_t)value;
        return true;
    case 3:
        if (depth >= 100) return false;
        while (*offset < size) {
            if (!AuthVarint(data, size, offset, &value) || !value || value > UINT32_MAX) return false;
            uint32_t nested = (uint32_t)value;
            if (!(nested >> 3)) return false;
            if ((nested & 7u) == 4) return (nested >> 3) == (tag >> 3);
            if (!AuthSkip(data, size, offset, nested, depth + 1)) return false;
        }
        return false;
    case 5:
        if (size - *offset < 4) return false;
        *offset += 4;
        return true;
    default: return false;
    }
}

static bool ParseAuthResponse(const IHS_Buffer *payload, CAuthenticationResponseMsg *message) {
    const uint8_t *data = IHS_BufferPointer(payload);
    size_t size = payload->size, offset = 0;
    while (offset < size) {
        uint64_t value;
        if (!AuthVarint(data, size, &offset, &value) || !value || value > UINT32_MAX) return false;
        uint32_t tag = (uint32_t)value, field = tag >> 3;
        if (!field) return false;
        if ((tag & 7u) == 0 && (field == 1 || field == 2)) {
            if (!AuthVarint(data, size, &offset, &value)) return false;
            /* Original enum reads int32: discard high bits before checking
             * its two known values. Unknown values retain the previous one. */
            uint32_t scalar = (uint32_t)value;
            if (scalar <= 1) {
                if (field == 1) {
                    message->has_result = true;
                    message->result = (CAuthenticationResponseMsg__AuthenticationResult)scalar;
                } else {
                    message->has_version = true;
                    message->version = (EStreamVersion)scalar;
                }
            }
        } else if (!AuthSkip(data, size, &offset, tag, 0)) return false;
    }
    return true;
}

void IHS_SessionChannelControlRequestAuthentication(IHS_SessionChannel *channel) {
    IHS_Session *session = channel->session;
    if (session->state.connectionState >= IHS_SessionConnectionStateAuthenticating) {
        return;
    }
    session->state.connectionState = IHS_SessionConnectionStateAuthenticating;
    CAuthenticationRequestMsg request = CAUTHENTICATION_REQUEST_MSG__INIT;
    PROTOBUF_C_SET_VALUE(request, version, k_EStreamVersionCurrent);
    PROTOBUF_C_SET_VALUE(request, steamid, session->info.steamId);
    if (session->networkTest.mode) {
        request.has_steamid = false;
        session->networkTest.phase = 3;
    }
    request.has_token = true;
    static const unsigned char plain[] = {'S', 't', 'e', 'a', 'm', ' ', 'I', 'n',
                                          '-', 'H', 'o', 'm', 'e', ' ', 'S', 't',
                                          'r', 'e', 'a', 'm', 'i', 'n', 'g'};
    uint8_t token[32];
    request.token.data = token;
    request.token.len = sizeof(token);
    if (IHS_SessionFrameHMACSHA256(session, plain, sizeof(plain), token, &request.token.len) != 0) {
        if (session->networkTest.mode)
            IHS_SessionNetworkTestFail(session);
        else
            IHS_SessionDisconnect(session);
        return;
    }
    IHS_SessionChannelControlSend(channel, k_EStreamControlAuthenticationRequest, (const ProtobufCMessage *) &request,
                                  IHS_PACKET_ID_NEXT);
}

void IHS_SessionChannelControlOnAuthentication(IHS_SessionChannel *channel, EStreamControlMessage type,
                                               IHS_Buffer *payload, const IHS_SessionPacketHeader *header) {
    IHS_UNUSED(header);
    assert(type == k_EStreamControlAuthenticationResponse);
    CAuthenticationResponseMsg message = CAUTHENTICATION_RESPONSE_MSG__INIT;
    if (!ParseAuthResponse(payload, &message)) {
        IHS_SessionLog(channel->session, IHS_LogLevelWarn, "Control", "Malformed CAuthenticationResponseMsg");
        return;
    }
    OnAuthenticationResponse(channel, &message);
}

static void OnAuthenticationResponse(IHS_SessionChannel *channel, const CAuthenticationResponseMsg *message) {
    IHS_Session *session = channel->session;
    // Ignore duplicate / late auth responses once we've moved on to negotiating or beyond.
    if (session->state.connectionState != IHS_SessionConnectionStateAuthenticating) {
        return;
    }
    if (!message->has_version || message->version != k_EStreamVersionCurrent ||
        message->result == CAUTHENTICATION_RESPONSE_MSG__AUTHENTICATION_RESULT__FAILED) {
        IHS_SessionLog(session, IHS_LogLevelError, "Session", "Authentication or stream version rejected");
        /* Stop on the RX owner without joining it from inside its handler.
         * Existing finalization joins producers and releases the socket. */
        if (session->networkTest.mode)
            IHS_SessionNetworkTestFail(session);
        else {
            IHS_SessionDisconnect(session);
            IHS_SessionClientTransportClosed(session);
        }
        return;
    }
    if (session->networkTest.mode) {
        IHS_SessionNetworkTestAuthenticated(session);
        return;
    }
    // Auth succeeded — the server will follow with NegotiationInit. Advance state so
    // OnNegotiationInit accepts the message and stale auth responses get dropped.
    session->state.connectionState = IHS_SessionConnectionStateNegotiating;
    IHS_SessionLog(session, IHS_LogLevelInfo, "Session", "Authenticated");
}
