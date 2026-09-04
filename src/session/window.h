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

#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "packet.h"
#include "frame.h"

typedef struct IHS_SessionPacketsWindow IHS_SessionPacketsWindow;

/**
 * Create packets window window
 * @param capacity Maximum packets capacity
 */
IHS_SessionPacketsWindow *IHS_SessionPacketsWindowCreate(uint16_t capacity);

void IHS_SessionPacketsWindowDestroy(IHS_SessionPacketsWindow *window);

bool IHS_SessionPacketsWindowAdd(IHS_SessionPacketsWindow *window, IHS_SessionPacket *packet);

bool IHS_SessionPacketsWindowPoll(IHS_SessionPacketsWindow *window, IHS_SessionFrame *frame);

/**
 * Discard all frames with timestamp difference between tail larger than `diff`
 * @param window
 * @param diff
 */
uint16_t IHS_SessionPacketsWindowDiscard(IHS_SessionPacketsWindow *window, uint32_t diff);

/**
 * Recycle every pending packet and reset head/tail. Unlike Discard(), this can't
 * fail to free anything: Discard() only advances past frame-head packets, so a
 * window holding nothing but orphaned fragments (frame head lost) is immovable.
 */
void IHS_SessionPacketsWindowReleaseAll(IHS_SessionPacketsWindow *window);

void IHS_SessionPacketsWindowReleaseFrame(IHS_SessionFrame *frame);

uint16_t IHS_SessionPacketsWindowAvailable(const IHS_SessionPacketsWindow *window);

uint16_t IHS_SessionPacketsWindowSize(const IHS_SessionPacketsWindow *window);

bool IHS_SessionPacketsWindowHasFrame(const IHS_SessionPacketsWindow *window);

/* NACK support (official protocol, docs/STEAMLINK_PROTOCOL_RE.md §9.1):
 * the lowest packet id whose delivery is still pending at the window head,
 * and a presence bitmap for ids from startId on (bit=1: held, bit=0: hole). */
uint16_t IHS_SessionPacketsWindowNextNeededPacketId(const IHS_SessionPacketsWindow *window);

/* True when at least one packet slot between head and tail is missing — the
 * official condition for emitting a gap NACK (in-flight fragments that have
 * not arrived yet leave used slots and do NOT count as holes). */
bool IHS_SessionPacketsWindowHasHole(const IHS_SessionPacketsWindow *window);

size_t IHS_SessionPacketsWindowHoleBitmap(const IHS_SessionPacketsWindow *window,
                                          uint16_t startId, uint8_t *bitmap, size_t maxPackets);
