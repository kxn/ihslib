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

#include "ch_stats.h"
#include "ihs_buffer.h"
#include "ihs_buffer_ext.h"
#include "session/session_pri.h"
#include <stdlib.h>

static const IHS_SessionChannelClass ChannelClass = {
        .received = IHS_SessionChannelReceivedPacketNoop,
        .instanceSize = sizeof(IHS_SessionChannel)
};

IHS_SessionChannel *IHS_SessionChannelStatsCreate(IHS_Session *session) {
    return IHS_SessionChannelCreate(&ChannelClass, session, IHS_SessionChannelTypeStats, IHS_SessionChannelIdStats,
                                    NULL);
}

bool IHS_SessionChannelStatsSend(IHS_SessionChannel *channel, EStreamStatsMessage type,
                                 const ProtobufCMessage *message, int32_t packetId) {
    /* Official SendStatsMessage uses Frame::Send, including fragmentation and
     * negotiated CRC; docs/STEAMLINK_PROTOCOL_RE.md §43. */
    IHS_SessionFrame frame;
    IHS_SessionChannelInitializeFrame(channel, &frame, IHS_SessionPacketTypeReliable, false, packetId);
    frame.body.maxCapacity = 0;
    IHS_BufferAppendUInt8(&frame.body, type);
    IHS_BufferAppendMessage(&frame.body, message);
    IHS_SendAdmission ret = IHS_SessionChannelQueueFrameResult(channel, &frame, true);
    IHS_SessionFrameClear(&frame, true);
    /* A failed local preparation can have consumed IDs. Match the existing
     * control-side protection: never continue that connection with an ID hole.
     * Socket errors occur later on the sender and do not enter this branch. */
    if (ret == IHS_SendAdmissionLocalFailure && !channel->session->transport.shutdown &&
        !channel->session->transport.detached) {
        if (channel->session->networkTest.mode)
            IHS_SessionNetworkTestFail(channel->session);
        else
            IHS_SessionDisconnect(channel->session);
    }
    return ret == IHS_SendAdmissionAccepted;
}
