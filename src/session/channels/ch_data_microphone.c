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

/* Official SendMicrophoneData -> Frame/Channel/Socket, protocol reference §114. */
#include "ch_data_microphone.h"
#include "ihs_buffer_ext.h"
#include "session/session_pri.h"

bool IHS_SessionMicrophoneSendFrame(IHS_Session *session, uint8_t channelId, uint16_t frameId,
                                     const uint8_t *data, size_t len, bool reliable) {
    /* Outgoing slots belong to the connection; a peer's downlink decoder may
     * use this same numeric ID. Never publish a mic in the RX consumer table. */
    IHS_SessionChannel channel = {.session = session, .id = channelId,
                                  .type = IHS_SessionChannelTypeDataMicrophone};
    IHS_SessionFrame frame;
    IHS_SessionChannelInitializeFrame(&channel, &frame,
        reliable ? IHS_SessionPacketTypeReliable : IHS_SessionPacketTypeUnreliable,
        false, IHS_PACKET_ID_NEXT);
    /* Input is bounded by the public producer. One frame can exceed one MTU. */
    frame.body.maxCapacity = 0;
    IHS_BufferAppendUInt8(&frame.body, k_EStreamDataPacket);
    IHS_BufferAppendUInt16LE(&frame.body, frameId);
    IHS_BufferAppendUInt32LE(&frame.body, 0);
    IHS_BufferAppendUInt16LE(&frame.body, 0);
    IHS_BufferAppendUInt32LE(&frame.body, 0);
    if (len)
        IHS_BufferAppendMem(&frame.body, data, len);
    bool sent = IHS_SessionChannelQueueFrame(&channel, &frame, reliable);
    IHS_BufferClear(&frame.body, true);
    return sent;
}
