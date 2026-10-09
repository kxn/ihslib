/*
 *  _____  _   _  _____  _  _  _
 * |_   _|| | | |/  ___|| |(_)| |     Steam
 *   | |  | |_| |\ `--. | | _ | |__     In-Home
 *   | |  |  _  | `--. \| || || '_ \      Streaming
 *  _| |_ | | | |/\__/ /| || || |_) |       Library
 *  \___/ \_| |_/\____/ |_||_||_.__/
 *
 * Copyright (c) 2026 Simone Caronni <https://github.com/scaronni>.
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

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ihs_udp.h"

/* The addresses depend on the machine; check what must hold on any of them. */
int main(void) {
    IHS_IPAddress addresses[8];
    size_t count = IHS_UDPBroadcastAddresses(addresses, 8);
    assert(count <= 8);
    for (size_t i = 0; i < count; i++) {
        static const uint8_t limited[4] = {0xFF, 0xFF, 0xFF, 0xFF}, any[4] = {0};
        assert(addresses[i].family == IHS_IPAddressFamilyIPv4);
        assert(memcmp(addresses[i].v4.data, limited, 4) != 0);
        assert(memcmp(addresses[i].v4.data, any, 4) != 0);
        for (size_t j = 0; j < i; j++) {
            assert(memcmp(addresses[i].v4.data, addresses[j].v4.data, 4) != 0);
        }
        printf("broadcast %u.%u.%u.%u\n", addresses[i].v4.data[0], addresses[i].v4.data[1],
               addresses[i].v4.data[2], addresses[i].v4.data[3]);
    }
    /* A full buffer never overflows. */
    assert(IHS_UDPBroadcastAddresses(addresses, 0) == 0);
    printf("udp broadcast tests OK (%zu addresses)\n", count);
    return 0;
}
