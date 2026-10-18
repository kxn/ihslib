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
#include <stdatomic.h>

#include "channel.h"
#include "ihs_thread.h"
#include "protobuf/remoteplay.pb-c.h"
#include "session/frame.h"
#include "session/fec.h"
#include "session/window.h"
#include "session/unreliable.h"

typedef struct IHS_SessionDataFrameHeader {
    bool timestampsLocal, receivedRecorded; /* local metadata, not data-header bytes */
    uint16_t id;
    uint32_t timestamp;
    uint16_t inputMark;
    uint32_t inputRecvTimestamp;
    uint32_t sendTimestamp, receiveTimestamp, receiveBegin; /* local transport metadata */
} IHS_SessionDataFrameHeader;

typedef struct IHS_SessionChannelData {
    IHS_SessionChannel base;
    IHS_UnreliableChannel *receive; /* borrowed from the connection */
    IHS_Mutex *windowLock; /* consumer FIFO lock; transport has receive->lock */
    IHS_Cond *windowCond;

    IHS_Thread *worker;
    atomic_bool interrupted;
    bool callerStarted, discardFrames; /* Startup completes before publishing an audio endpoint. */
    IHS_Mutex *lock;

    /* All accepted complete payloads cross to the decoder worker through this
     * bounded FIFO. Transport windows and FEC groups belong to the connection. */
    struct IHS_FECFrameNode *fecHead, *fecTail;
    size_t fecQueuedBytes, fecQueuedCount, fecQueueLimit;
    unsigned fecQueueCapacity;
} IHS_SessionChannelData;

typedef struct IHS_SessionChannelDataClass {
    IHS_SessionChannelClass base;

    bool (*start)(struct IHS_SessionChannel *channel);

    void (*dataFrame)(struct IHS_SessionChannel *channel, const IHS_SessionDataFrameHeader *header,
                      IHS_Buffer *body);

    void (*stop)(struct IHS_SessionChannel *channel);

    bool startOnCaller; /* Start on lifecycle actor; stop only after worker join/purge. */
} IHS_SessionChannelDataClass;

#define IHS_SESSION_DATA_FRAME_HEADER_SIZE 12

IHS_SessionChannel *IHS_SessionChannelDataCreate(const IHS_SessionChannelDataClass *cls,
                                                 IHS_Session *session, IHS_SessionChannelType type,
                                                 IHS_SessionChannelId id, const void *config);

bool IHS_SessionChannelDataInit(IHS_SessionChannel *channel, uint16_t windowCapacity);

void IHS_SessionChannelDataDeinit(IHS_SessionChannel *channel);

void IHS_SessionChannelDataReceived(IHS_SessionChannel *channel, IHS_SessionPacket *packet);

void IHS_SessionChannelDataLost(IHS_SessionChannel *channel);

void IHS_SessionChannelDataStopped(IHS_SessionChannel *channel);

size_t IHS_SessionChannelDataFrameHeaderParse(IHS_SessionDataFrameHeader *header,
                                              const IHS_Buffer *data);

/* Internal protocol step, serialized by receive->lock. UINT32_MAX means wait for
 * more input; otherwise retry after waitTicks. Ready packets transfer to caller. */
bool IHS_SessionChannelDataBuildFeedback(IHS_SessionChannelData *, uint32_t now,
                                         IHS_SessionPacket *, uint32_t *waitTicks);

/* Source OnDataPacket: latch offset/record receive sampling before decoder handoff. */
void IHS_SessionChannelDataPrepareFrame(IHS_SessionChannel *, IHS_SessionFrame *);

/* Transfer an ordered, reassembled complete frame to the decoder worker. */
void IHS_SessionChannelDataReceiveReliableFrame(IHS_SessionChannel *, IHS_SessionFrame *);

/* RX batch-end protocol update, independent of decoder progress. */
void IHS_SessionChannelDataUpdateAt(IHS_SessionChannelData *, uint32_t tick);
/* Decoder FIFO polling only. Hold windowLock; missing is always zero. */
bool IHS_SessionChannelDataPollLocked(IHS_SessionChannelData *, IHS_SessionFrame *, uint16_t *missing);
/* RX owner, outside Base/state locks: exclude and join decoder, purge only its
 * complete-frame FIFO. Connection receive windows/FEC groups stay alive. */
void IHS_SessionChannelDataJoinAndPurge(IHS_SessionChannelData *, bool restarting);
void IHS_SessionChannelDataRestartWorker(IHS_SessionChannelData *);

/* Complete-frame consumer used by the data worker, outside windowLock. */
void IHS_SessionChannelDataReceiveFrame(IHS_SessionChannelData *, IHS_SessionFrame *);
