/*
 *  _____  _   _  _____  _  _  _
 * |_   _|| | | |/  ___|| |(_)| |     Steam
 *   | |  | |_| |\ `--. | | _ | |__     In-Home
 *   | |  |  _  | `--. \| || || '_ \      Streaming
 *  _| |_ | | | |/\__/ /| || || |_) |       Library
 *  \___/ \_| |_/\____/ |_||_||_.__/
 *
 * Copyright (c) 2026 Mariotaku <https://github.com/mariotaku>.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 */

/* Android ARM64 1.3.32: backend loss requests recovery without an
 * unconditional reset; a sequence gap still starts strict keyframe wait. */

#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

#include "session/session_pri.h"
#include "session/channels/ch_data.h"
#include "session/channels/video/ch_data_video.h"
#include "endianness.h"
#include "ihs_queue.h"
#include "protobuf/pb_utils.h"

#include "test_session.h"

/** Mirrors session.c — the queue item layout is private there. */
typedef struct IHS_QueueItem {
    IHS_SessionPacket packet;
    bool retransmit;
} QueuedPacket;

#define VIDEO_HEADER_SIZE 7
#define MAX_SUBMITTED     16

static struct {
    uint8_t data[512];
    size_t len;
} submitted[MAX_SUBMITTED];

static size_t submittedCount = 0;

/** Result the next submit returns, then it falls back to OK. Lets a test fail one decode. */
static IHS_StreamVideoSubmitResult nextSubmitResult = IHS_StreamVideoSubmitOK;

static IHS_StreamVideoSubmitResult OnSubmit(IHS_Session *session, uint16_t frameId, IHS_Buffer *data, IHS_StreamVideoFrameFlag flags,
                                            void *context) {
    (void) frameId;
    (void) session;
    (void) context;
    (void) flags;
    assert(submittedCount < MAX_SUBMITTED);
    assert(data->size <= sizeof(submitted[0].data));
    memcpy(submitted[submittedCount].data, IHS_BufferPointer(data), data->size);
    submitted[submittedCount].len = data->size;
    submittedCount++;
    IHS_StreamVideoSubmitResult result = nextSubmitResult;
    nextSubmitResult = IHS_StreamVideoSubmitOK;
    return result;
}

static pthread_mutex_t readyLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t readyCond = PTHREAD_COND_INITIALIZER;
static bool initialized;
static unsigned overflowReports;
static uint32_t firstOverflow, lastOverflow;
static _Atomic uint64_t fakeNow;
static _Atomic bool oracleTicks;
static _Atomic uint32_t oracleNow;
uint32_t __real_IHS_SessionPacketTimestamp(void);
uint32_t __wrap_IHS_SessionPacketTimestamp(void) {
    if (oracleTicks)
        return oracleNow;
    uint64_t millis = fakeNow;
    if (millis)
        return IHS_SESSION_PACKET_TIMESTAMP_FROM_MILLIS(millis);
    return __real_IHS_SessionPacketTimestamp();
}
uint64_t __real_IHS_TimerNow(void);
uint64_t __wrap_IHS_TimerNow(void) {
    uint64_t millis = fakeNow;
    return millis ? millis : __real_IHS_TimerNow();
}
static int OnStart(IHS_Session *s, const IHS_StreamVideoConfig *config, void *context) {
    (void)s; (void)config; (void)context;
    return 0;
}
bool __real_IHS_SessionSendControlMessage(IHS_Session *, EStreamControlMessage, const ProtobufCMessage *);
bool __wrap_IHS_SessionSendControlMessage(IHS_Session *s, EStreamControlMessage type,
                                          const ProtobufCMessage *message) {
    if (type == k_EStreamControlVideoOverflow) {
        const CStreamVideoOverflowMsg *m = (const CStreamVideoOverflowMsg *)message;
        assert(m->has_first_frame_id && m->has_last_frame_id);
        firstOverflow = m->first_frame_id;
        lastOverflow = m->last_frame_id;
        overflowReports++;
        return true;
    }
    if (type != k_EStreamControlVideoDecoderInfo)
        return __real_IHS_SessionSendControlMessage(s, type, message);
    const CVideoDecoderInfoMsg *decoder = (const CVideoDecoderInfoMsg *)message;
    assert(decoder->info && !strcmp(decoder->info, "test decoder"));
    assert(decoder->has_threads && decoder->threads == 2);
    pthread_mutex_lock(&readyLock);
    initialized = true;
    pthread_cond_signal(&readyCond);
    pthread_mutex_unlock(&readyLock);
    return true;
}
static const IHS_StreamVideoCallbacks videoCallbacks = {
        .start = OnStart,
        .submit = OnSubmit,
};

/* ------------------------------------------------------------------ harness */

static IHS_Session *session = NULL;
static IHS_SessionChannel *channel = NULL;

/** Feed one data frame. `payload` is appended raw, so assembled output compares byte for byte. */
static void Feed(uint16_t frameId, uint32_t timestamp, uint16_t sequence, uint8_t flags,
                 uint16_t subFrameStart, uint16_t subFrameEnd, const uint8_t *payload, size_t payloadLen) {
    IHS_Buffer body = IHS_BUFFER_INIT(VIDEO_HEADER_SIZE + payloadLen, 4096);
    uint8_t head[VIDEO_HEADER_SIZE];
    size_t offset = 0;
    offset += IHS_WriteUInt16LE(&head[offset], sequence);
    head[offset++] = flags;
    offset += IHS_WriteUInt16LE(&head[offset], subFrameStart);
    offset += IHS_WriteUInt16LE(&head[offset], subFrameEnd);
    assert(offset == VIDEO_HEADER_SIZE);
    IHS_BufferAppendMem(&body, head, VIDEO_HEADER_SIZE);
    if (payloadLen > 0) {
        IHS_BufferAppendMem(&body, payload, payloadLen);
    }

    IHS_SessionDataFrameHeader header = {.id = frameId, .timestamp = timestamp};
    const IHS_SessionChannelDataClass *cls = (const IHS_SessionChannelDataClass *) channel->cls;
    cls->dataFrame(channel, &header, &body);
    IHS_BufferClear(&body, true);
}

/** Number of keyframe requests queued since the last call. */
static size_t TakeKeyframeRequests(void) {
    size_t count = 0;
    IHS_QueueItem *item;
    while ((item = IHS_QueuePoll(session->sendQueue)) != NULL) {
        QueuedPacket *queued = (QueuedPacket *) item;
        assert(IHS_BufferPointerAt(&queued->packet.body, 0)[0] == k_EStreamDataLost);
        IHS_SessionPacketClear(&queued->packet, true);
        IHS_QueueItemFree(item);
        count++;
    }
    return count;
}

static void Reset(void) {
    submittedCount = 0;
    nextSubmitResult = IHS_StreamVideoSubmitOK;
    overflowReports = 0;
    TakeKeyframeRequests();
}

static void SetUp(void) {
    IHS_Init();
    session = IHS_TestSessionCreate();
    char decoder[] = "test decoder";
    IHS_StreamClientCapabilities capabilities = {.decoderInfo = decoder, .decoderThreads = 2};
    assert(IHS_SessionSetClientCapabilities(session, &capabilities));
    memset(decoder, 'x', sizeof(decoder) - 1); /* session owns its copy */
    IHS_SessionSetVideoCallbacks(session, &videoCallbacks, NULL);
    CStartVideoDataMsg message = CSTART_VIDEO_DATA_MSG__INIT;
    message.channel = 3;
    PROTOBUF_C_SET_VALUE(message, codec, k_EStreamVideoCodecH264);
    PROTOBUF_C_SET_VALUE(message, width, 1280);
    PROTOBUF_C_SET_VALUE(message, height, 720);
    channel = IHS_SessionChannelDataVideoCreate(session, &message);
    assert(channel != NULL);
    pthread_mutex_lock(&readyLock);
    while (!initialized)
        pthread_cond_wait(&readyCond, &readyLock);
    pthread_mutex_unlock(&readyLock);
    Reset();
}

static void TearDown(void) {
    // The data channel spins up a worker thread at init, and its deinit asserts the thread has been
    // interrupted first. That is what the `stopped` hook does.
    channel->cls->stopped(channel);
    IHS_SessionChannelDestroy(channel);
    TakeKeyframeRequests();
    IHS_SessionDestroy(session);
    IHS_Quit();
}

/* ------------------------------------------------------------------ cases */

static void test_decoder_reported_loss_requests_keyframe(void) {
    Reset();
    const uint8_t payload[] = {0x55, 0x66};
    const uint8_t after[] = {0x77, 0x88};

    Feed(1, 1000, 0, VideoFrameFlagKeyFrame | VideoFrameFlagSubFrameAdvance | VideoFrameFlagFrameFinish,
         0, 3, payload, sizeof(payload));
    assert(submittedCount == 1);
    assert(TakeKeyframeRequests() == 0);

    // Sequence is intact — the only thing wrong is that the decoder could not decode this frame.
    nextSubmitResult = IHS_StreamVideoSubmitReportLost;
    Feed(2, 2000, 1, VideoFrameFlagSubFrameAdvance | VideoFrameFlagFrameFinish, 0, 3, after, sizeof(after));
    assert(submittedCount == 2 && "the failing frame itself still reaches the decoder");
    assert(TakeKeyframeRequests() == 1);
    Feed(3, 3000, 2, VideoFrameFlagSubFrameAdvance | VideoFrameFlagFrameFinish, 0, 3, after, sizeof(after));
    assert(submittedCount == 3); // decoder state has not been destroyed

    // A keyframe clears the wait and streaming resumes.
    const uint8_t recovered[] = {0x99};
    Feed(4, 4000, 3, VideoFrameFlagKeyFrame | VideoFrameFlagSubFrameAdvance | VideoFrameFlagFrameFinish,
         0, 3, recovered, sizeof(recovered));
    assert(submittedCount == 4);
    assert(submitted[3].data[0] == 0x99);
}

static void test_soft_backlog_and_strict_gap(void) {
    Reset();
    const uint8_t payload[] = {0x42};
    const uint8_t finish = VideoFrameFlagSubFrameAdvance | VideoFrameFlagFrameFinish;
    fakeNow = 1000;
    Feed(10, 0, 20, VideoFrameFlagKeyFrame | finish, 0, 0, payload, 1);
    Feed(11, 1000, 21, VideoFrameFlagSubFrameAdvance, 2, 3, payload, 1);
    Feed(11, 11000, 22, finish, 4, 5, payload, 1);
    assert(submittedCount == 1 && TakeKeyframeRequests() == 1);
    assert(overflowReports && firstOverflow == 11 && lastOverflow == 11);
    fakeNow = 1100;
    Feed(11, 11500, 23, VideoFrameFlagSubFrameAdvance, 0, 1, payload, 1);
    assert(submittedCount == 2);         // soft sync accepts the completed frame
    assert(TakeKeyframeRequests() == 0); // exact 100ms is not expired
    fakeNow = 1101;
    Feed(12, 12000, 24, finish, 0, 0, payload, 1);
    assert(submittedCount == 3 && TakeKeyframeRequests() == 1);
    Feed(13, 13000, 26, finish, 0, 0, payload, 1);
    assert(submittedCount == 3 && TakeKeyframeRequests() == 0);
    Feed(14, 14000, 27, finish, 0, 0, payload, 1);
    assert(submittedCount == 3 && TakeKeyframeRequests() == 0);
    Feed(15, 15000, 28, VideoFrameFlagKeyFrame | finish, 0, 0, payload, 1);
    assert(submittedCount == 4); // no retry delay after keyframe
    fakeNow = 0;
}

static void test_overflow_frame_ranges(void) {
    Reset();
    const uint8_t payload = 0x42;
    const uint8_t finish = VideoFrameFlagSubFrameAdvance | VideoFrameFlagFrameFinish;
    Feed(40, 0, 400, VideoFrameFlagKeyFrame | finish, 0, 0, &payload, 1);
    Feed(41, 1000, 401, 0, 2, 3, &payload, 1);
    assert(overflowReports == 0);
    Feed(42, 1001, 402, 0, 2, 3, &payload, 1);
    assert(overflowReports == 1 && firstOverflow == 41 && lastOverflow == 42);
    Feed(42, 1002, 403, 0, 2, 3, &payload, 1); // equal inclusive range = 2
    assert(overflowReports == 2);
    Feed(43, 1003, 404, 0, 2, 3, &payload, 1); // larger range = 3
    assert(overflowReports == 3);
    Feed(41, 1004, 405, 0, 2, 3, &payload, 1); // range 1 < 3: suppress but store
    assert(overflowReports == 3);
    Feed(41, 1005, 406, 0, 2, 3, &payload, 1); // equal to the stored 1: report
    assert(overflowReports == 4 && firstOverflow == 41 && lastOverflow == 41);
    assert(submittedCount == 1 && TakeKeyframeRequests() == 0);
}

static void test_overflow_wrap_and_reversed_timestamps(void) {
    Reset();
    const uint8_t payload = 0x42;
    const uint8_t finish = VideoFrameFlagSubFrameAdvance | VideoFrameFlagFrameFinish;
    Feed(65534, 0, 700, VideoFrameFlagKeyFrame | finish, 0, 0, &payload, 1);
    Feed(65535, 2000, 701, 0, 2, 3, &payload, 1);
    Feed(0, 1999, 702, 0, 2, 3, &payload, 1);
    assert(overflowReports == 0); // signed inclusive frame-ID range is negative
    Feed(1, 1998, 703, 0, 2, 3, &payload, 1);
    assert(overflowReports == 1 && firstOverflow == 65535 && lastOverflow == 1);
    Feed(2, 0, 704, VideoFrameFlagKeyFrame | finish, 0, 0, &payload, 1);
    Feed(3, 2000, 705, 0, 2, 3, &payload, 1);
    Feed(4, 1999, 706, 0, 2, 3, &payload, 1);
    assert(overflowReports == 2 && firstOverflow == 3 && lastOverflow == 4);
    assert(TakeKeyframeRequests() == 0);
}

static void test_ready_pending_frames_do_not_wait_for_another_packet(void) {
    Reset();
    const uint8_t key = 0x40, a = 0x41, b = 0x42, c = 0x43;
    const uint8_t finish = VideoFrameFlagSubFrameAdvance | VideoFrameFlagFrameFinish;
    Feed(60, 0, 600, VideoFrameFlagKeyFrame | finish, 0, 0, &key, 1);
    Feed(61, 1000, 601, finish, 2, 3, &b, 1);
    Feed(62, 1001, 602, finish, 0, 0, &c, 1);
    assert(submittedCount == 1);
    Feed(61, 1002, 603, VideoFrameFlagSubFrameAdvance, 0, 1, &a, 1);
    assert(submittedCount == 3); // no later packet is needed to submit frame 62
    assert(submitted[1].len == 2 && submitted[1].data[0] == a && submitted[1].data[1] == b);
    assert(submitted[2].len == 1 && submitted[2].data[0] == c);
}

static void test_pending_storage_bound(void) {
    Reset();
    const uint8_t payload = 0x42;
    const uint8_t finish = VideoFrameFlagSubFrameAdvance | VideoFrameFlagFrameFinish;
    Feed(20, 0, 100, VideoFrameFlagKeyFrame | finish, 0, 0, &payload, 1);
    for (unsigned i = 0; i < 256; i++)
        Feed(21, 1000, 101 + i, 0, 2, 3, &payload, 1);
    assert(submittedCount == 1 && TakeKeyframeRequests() == 0);
    Feed(21, 1000, 357, 0, 2, 3, &payload, 1);
    assert(submittedCount == 1 && TakeKeyframeRequests() == 1);
    Feed(22, 1000, 358, finish, 0, 0, &payload, 1);
    assert(submittedCount == 1);
    Feed(23, 1000, 359, VideoFrameFlagKeyFrame | finish, 0, 0, &payload, 1);
    assert(submittedCount == 2);
}

int main(int argc, char **argv) {
    SetUp();
    if (argc == 2 && !strcmp(argv[1], "--oracle")) {
        oracleTicks = true;
        char line[256];
        unsigned tick, id, seq, flags;
        const uint8_t payload = 0x42;
        while (fgets(line, sizeof(line), stdin)) {
            char mode;
            if (sscanf(line, "M %c %u", &mode, &tick) == 2) {
                oracleNow = tick;
                Feed(10, 0, 0, VideoFrameFlagKeyFrame | VideoFrameFlagFrameFinish, 0, 0, &payload,
                     1);
                if (mode == 'H')
                    Feed(11, 1000, 2, VideoFrameFlagFrameFinish, 0, 0, &payload, 1);
                if (mode == 'B') {
                    Feed(11, 1000, 1, VideoFrameFlagSubFrameAdvance, 2, 3, &payload, 1);
                    Feed(11, 11000, 2, VideoFrameFlagSubFrameAdvance | VideoFrameFlagFrameFinish, 4,
                         5, &payload, 1);
                    Feed(11, 11500, 3, VideoFrameFlagSubFrameAdvance, 0, 1, &payload, 1);
                }
                Reset();
            } else if (sscanf(line, "F %u %u %u %u", &tick, &id, &seq, &flags) == 4) {
                oracleNow = tick;
                size_t before = submittedCount;
                Feed(id, 20000, seq, flags, 0, 0, &payload, 1);
                printf("{\"submitted\":%s,\"lost\":%zu}\n",
                       submittedCount > before ? "true" : "false", TakeKeyframeRequests());
            } else if (sscanf(line, "R %u", &flags) == 1) {
                nextSubmitResult = flags;
            } else
                assert(!"bad oracle command");
        }
        oracleTicks = false;
        TearDown();
        return 0;
    }
    test_decoder_reported_loss_requests_keyframe();
    test_soft_backlog_and_strict_gap();
    test_overflow_frame_ranges();
    test_overflow_wrap_and_reversed_timestamps();
    test_ready_pending_frames_do_not_wait_for_another_packet();
    test_pending_storage_bound();
    TearDown();
    printf("decoder loss tests OK\n");
    return 0;
}
