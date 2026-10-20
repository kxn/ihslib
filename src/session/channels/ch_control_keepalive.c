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

#include "ch_control.h"

/* Original HandleStreaming 0x7a7140; normal Start seeds the timestamp.
 * A zero timestamp is a sentinel, and the 10-second comparison is strict.
 * See protocol §49–50: no independent timer or immediate normal first send. */
void IHS_SessionChannelControlUpdateHeartbeat(IHS_SessionChannel *channel, uint32_t now) {
    IHS_SessionChannelControl *control = (void *)channel;
    uint32_t last = control->keepAliveTimestamp;
    /* Match ARM64 sub/cmn/b.pl, including its 32-bit half-range edges. */
    if (last && (int32_t)(last - now + 0xa0000u) >= 0)
        return;
    CKeepAliveMsg message = CKEEP_ALIVE_MSG__INIT;
    IHS_SessionChannelControlSend(channel, k_EStreamControlKeepAlive,
                                  (const ProtobufCMessage *)&message, IHS_PACKET_ID_NEXT);
    /* Original records an attempt, including a rejected/failed send. */
    control->keepAliveTimestamp = now;
}
