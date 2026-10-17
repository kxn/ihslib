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
#include <stddef.h>

#include "buffer.h"

typedef struct IHS_Session IHS_Session;

typedef enum IHS_StreamAudioCodec {
    IHS_StreamAudioCodecNone = 0,
    IHS_StreamAudioCodecRaw = 1,
    IHS_StreamAudioCodecVorbis = 2,
    IHS_StreamAudioCodecOpus = 3,
    IHS_StreamAudioCodecMP3 = 4,
    IHS_StreamAudioCodecAAC = 5,
} IHS_StreamAudioCodec;

typedef struct IHS_StreamAudioConfig {
    uint32_t channels;
    uint32_t frequency;
    IHS_StreamAudioCodec codec;
    uint8_t *codecData;
    size_t codecDataLen;
} IHS_StreamAudioConfig;

/* Extended media header, after the receive-time clock conversion. The payload
 * remains encoded codec data. Values are valid only during submitFrame. */
typedef struct IHS_StreamAudioFrameInfo {
    uint16_t id;
    uint32_t timestamp; /* local 16.16 seconds, captured before the decoder FIFO */
} IHS_StreamAudioFrameInfo;

/* start runs synchronously on the control-dispatch caller, before pending
 * media is replayed. A nonzero result disables this audio decoder, preserving
 * the session and configured media endpoint; stop is not called for failure.
 * If OS worker creation fails after backend success, retain the backend and
 * bounded pending queue until stop, matching the original decoder lifecycle.
 * submit/submitFrame run on the joined audio worker. stop runs on the teardown
 * caller only after that worker has stopped and its queued frames are purged.
 * These callbacks must not synchronously destroy the session or reconfigure
 * the same media endpoint. */
typedef struct IHS_StreamAudioCallbacks {
    int (*start)(IHS_Session *session, const IHS_StreamAudioConfig *config, void *context);

    int (*submit)(IHS_Session *session, IHS_Buffer *data, void *context);

    void (*stop)(IHS_Session *session, void *context);

    /* Initialize this callbacks struct to zero and rebuild callers with this
     * header. Preferred when supplied; otherwise the original submit is used.
     * IDs allow the application decoder to reproduce official Opus PLC/FEC. */
    int (*submitFrame)(IHS_Session *session, const IHS_StreamAudioFrameInfo *info,
                       IHS_Buffer *data, void *context);

    /* Optional player/device stage. Runs synchronously before codec factory dispatch,
     * including unsupported codecs. Nonzero skips decoder creation; the player
     * owns any retained device/jitter state until the next prepare or its own
     * shutdown. When present, start/stop manage the decoder only; stop follows
     * worker join/purge and must preserve the player's queued audio tail.
     * Without prepare, the legacy combined start/stop contract is retained.
     * Zero-initialize this struct and rebuild callers with this header. */
    int (*prepare)(IHS_Session *session, const IHS_StreamAudioConfig *config, void *context);
} IHS_StreamAudioCallbacks;

/**
 * Microphone (client → server) streaming. The library does not capture or encode audio;
 * it only handles the protocol framing. The user provides:
 *   - `start`: called when the server requests a microphone stream. The config carries
 *     the negotiated codec / sample rate / channel count chosen by Steam. Return 0 on
 *     success; non-zero aborts the channel (no frames will be sent). Open and configure
 *     the platform capture device here.
 *   - `stop`: called when the server requests teardown (or the session is closing). Close
 *     the capture device here. No frames may be passed to IHS_SessionSendMicrophoneData
 *     after this returns.
 *
 * After `start` succeeds the user's capture thread calls IHS_SessionSendMicrophoneData
 * once per encoded frame. Steam's reference client uses libopus at 480 samples per
 * channel per frame for Opus, or raw 32-bit float PCM passthrough for codec=Raw.
 */
typedef struct IHS_StreamMicrophoneCallbacks {
    int (*start)(IHS_Session *session, const IHS_StreamAudioConfig *config, void *context);

    void (*stop)(IHS_Session *session, void *context);
} IHS_StreamMicrophoneCallbacks;
