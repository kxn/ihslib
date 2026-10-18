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

/* accepted and newly inserted are distinct for old/duplicate packets (§58). */
bool IHS_SessionPacketsWindowAddEx(IHS_SessionPacketsWindow *, IHS_SessionPacket *, bool *inserted);

bool IHS_SessionPacketsWindowPoll(IHS_SessionPacketsWindow *window, IHS_SessionFrame *frame);

/**
 * Discard all frames with timestamp difference between tail larger than `diff`
 * @param window
 * @param diff
 */
uint16_t IHS_SessionPacketsWindowDiscard(IHS_SessionPacketsWindow *window, uint32_t diff);
uint16_t IHS_SessionPacketsWindowDiscardEx(IHS_SessionPacketsWindow *, uint32_t diff, uint16_t *missing);

/**
 * Recycle every pending packet and reset head/tail, regardless of its age or
 * whether it contains a complete frame. Used for bounded-capacity recovery.
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

/* Reliable streams start at a known ID and never discard orphan fragments. */
IHS_SessionPacketsWindow *IHS_SessionPacketsWindowCreateReliable(uint16_t capacity, uint16_t firstId);
uint16_t IHS_SessionPacketsWindowContiguousId(const IHS_SessionPacketsWindow *window);

/* Both reliable NACK and UnreliableNack use the first PRESENT successor as bitmap base, while the
 * watermark stops before the first hole. Snapshot while holding windowLock. */
typedef struct IHS_MediaFeedback {
    uint16_t base, confirmed;
    uint8_t bitmap[40];
    size_t bitmapBytes;
    uint32_t receiveTimestamp;
    uint8_t retransmitCount;
} IHS_MediaFeedback;
bool IHS_SessionPacketsWindowMediaFeedback(const IHS_SessionPacketsWindow *, IHS_MediaFeedback *);

/* Fresh media channels, like reliable channels, start at packet ID zero. */
IHS_SessionPacketsWindow *IHS_SessionPacketsWindowCreateMedia(uint16_t capacity);

/* Snapshot a reliable gap and whether any present successor is older than 65
 * local clock ticks. This age scan is not capped at the bitmap's 320 bits. */
bool IHS_SessionPacketsWindowReliableFeedback(const IHS_SessionPacketsWindow *, uint32_t now,
                                               IHS_MediaFeedback *, bool *mature);
