/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Official CNetworkTest producer/loop, protocol reference §116–119. */
#include "network_test.h"
#include "channels/ch_control.h"
#include "channels/ch_data.h"
#include "channels/ch_stats.h"
#include "frame_stats.h"
#include "session_pri.h"
#include "unreliable.h"
#include <limits.h>
#include <string.h>

bool IHS_SessionConfigureNetworkTest(IHS_Session *s, IHS_NetworkTestMode mode,
                                     uint32_t targetKbps) {
    if (!s || (mode != IHS_NetworkTestFixed && mode != IHS_NetworkTestBandwidth) || !targetKbps ||
        targetKbps > INT32_MAX ||
        (s->info.sessionKeyLen != 16 && s->info.sessionKeyLen != 32))
        return false;
    IHS_BaseLock(&s->base);
    bool ok = !s->base.worker && !s->base.interrupted &&
              s->state.connectionState == IHS_SessionConnectionStateUnconnected;
    if (ok) {
        s->networkTest = (IHS_SessionNetworkTest){
            .mode = mode, .requestedKbps = targetKbps, .currentKbps = targetKbps};
    }
    IHS_BaseUnlock(&s->base);
    return ok;
}
bool IHS_SessionNetworkTestBegin(IHS_Session *s) {
    IHS_BaseLock(&s->base);
    if (s->networkTest.mode) {
        s->networkTest.status = IHS_NetworkTestRunning;
        s->networkTest.phase = 1;
        s->mediaNetworkStats.recordFull = true;
        IHS_MediaNetworkStatsStart(&s->mediaNetworkStats);
        /* Official CNetworkTest::BConnect @caa014..24 activates channel3
         * with 6553 ticks, overriding the generic constructor's 65. This
         * happens before Connect and does not depend on StartVideoData. */
        IHS_UnreliableChannel *rx = IHS_SessionUnreliableGet(s, 3, 2048);
        if (!rx) {
            s->networkTest.status = IHS_NetworkTestFailed;
            s->networkTest.phase = 0;
            s->mediaNetworkStats.valid = false;
            IHS_BaseUnlock(&s->base);
            return false;
        }
        IHS_OutgoingRegister(s, 3);
        rx->maxReceiveDelay = 6553;
    }
    IHS_BaseUnlock(&s->base);
    return true;
}
void IHS_SessionNetworkTestConnected(IHS_Session *s, uint32_t now) {
    if (!s->networkTest.mode)
        return;
    IHS_BaseLock(&s->base);
    s->networkTest.phase = 2;
    s->networkTest.handshakeTick = now;
    IHS_BaseUnlock(&s->base);
}
static bool keep_alive(IHS_Session *s) {
    CKeepAliveMsg msg = CKEEP_ALIVE_MSG__INIT;
    bool ok =
        IHS_SessionSendControlMessage(s, k_EStreamControlKeepAlive, (const ProtobufCMessage *)&msg);
    s->networkTest.keepAliveTick = IHS_SessionPacketTimestamp();
    return ok;
}
void IHS_SessionNetworkTestAuthenticated(IHS_Session *s) {
    IHS_BaseLock(&s->base);
    IHS_SessionNetworkTest *n = &s->networkTest;
    n->phase = 4;
    s->state.connectionState = IHS_SessionConnectionStateNegotiating;
    for (unsigned i = 0; i < 3; ++i) {
        if (!keep_alive(s)) {
            IHS_BaseUnlock(&s->base);
            IHS_SessionNetworkTestFail(s);
            return;
        }
    }
    CStartNetworkTestMsg msg = CSTART_NETWORK_TEST_MSG__INIT;
    msg.has_frames = msg.has_framerate = msg.has_bitrate_kbps = true;
    msg.has_burst_bitrate_kbps = msg.has_bandwidth_test = true;
    msg.frames = 600;
    msg.framerate = 60;
    msg.bitrate_kbps = (int32_t)n->requestedKbps;
    msg.burst_bitrate_kbps = 0;
    msg.bandwidth_test = n->mode == IHS_NetworkTestBandwidth;
    if (!IHS_SessionSendControlMessage(s, k_EStreamControlStartNetworkTest,
                                       (const ProtobufCMessage *)&msg)) {
        IHS_BaseUnlock(&s->base);
        IHS_SessionNetworkTestFail(s);
        return;
    }
    n->lastData = IHS_SessionPacketTimestamp();
    IHS_BaseUnlock(&s->base);
}
void IHS_SessionNetworkTestRecord(IHS_Session *s, IHS_SessionFrame *frame) {
    if (frame->body.size < 1 + IHS_SESSION_DATA_FRAME_HEADER_SIZE ||
        *IHS_BufferPointer(&frame->body) != k_EStreamDataPacket)
        return;
    IHS_Buffer body = frame->body;
    IHS_BufferOffsetBy(&body, 1);
    IHS_SessionDataFrameHeader h = {0};
    if (!IHS_SessionChannelDataFrameHeaderParse(&h, &body))
        return;
    IHS_BaseLock(&s->base);
    uint32_t sent = frame->header.sendTimestamp - IHS_StreamClockOffset(&s->clock);
    IHS_MediaNetworkStatsRecord(&s->mediaNetworkStats, h.id, sent, frame->header.receiveTimestamp);
    s->networkTest.lastData = frame->header.receiveTimestamp;
    if (s->mediaNetworkStats.total >= 600 && s->networkTest.mode != IHS_NetworkTestBandwidth)
        s->networkTest.phase = 5;
    IHS_BaseUnlock(&s->base);
}
void IHS_SessionNetworkTestTarget(IHS_Session *s, uint32_t kbps) {
    IHS_BaseLock(&s->base);
    IHS_SessionNetworkTest *n = &s->networkTest;
    n->previousKbps = n->previousKbps ? n->currentKbps : kbps;
    n->currentKbps = kbps;
    s->mediaNetworkStats.targetKbps = kbps;
    IHS_BaseUnlock(&s->base);
}
static void close_test(IHS_Session *s) {
    /* RX owner; no decoder worker exists in this protocol mode. */
    IHS_SessionTransportDisconnect(s);
    IHS_SessionTransportClose(s);
    IHS_BaseLock(&s->base);
    s->networkTest.lastData = s->networkTest.handshakeTick = 0;
    s->networkTest.frameEventsTick = s->networkTest.keepAliveTick = 0;
    IHS_SessionChannelControl *control = (void *)s->channels[1];
    IHS_MutexLock(control->sendLock);
    control->sendEncryptSequence = control->recvEncryptSequence = 0;
    IHS_MutexUnlock(control->sendLock);
    IHS_BaseUnlock(&s->base);
    IHS_SessionInterrupt(s);
}
void IHS_SessionNetworkTestFail(IHS_Session *s) {
    IHS_BaseLock(&s->base);
    s->networkTest.phase = 0;
    IHS_BaseUnlock(&s->base);
    close_test(s);
}
void IHS_SessionNetworkTestCancel(IHS_Session *s) {
    IHS_BaseLock(&s->base);
    if (s->networkTest.mode && s->networkTest.status == IHS_NetworkTestRunning) {
        s->networkTest.status = IHS_NetworkTestCancelled;
        s->networkTest.phase = 0;
    }
    IHS_BaseUnlock(&s->base);
}
void IHS_SessionNetworkTestClosed(IHS_Session *s) {
    IHS_BaseLock(&s->base);
    if (s->networkTest.mode)
        s->networkTest.phase = 0;
    IHS_BaseUnlock(&s->base);
}
void IHS_SessionNetworkTestFinalize(IHS_Session *s) {
    IHS_BaseLock(&s->base);
    if (s->networkTest.mode) {
        if (s->networkTest.status == IHS_NetworkTestRunning)
            s->networkTest.status = IHS_NetworkTestFailed;
        s->networkTest.phase = 0;
        s->networkTest.lastData = s->networkTest.handshakeTick = 0;
        s->networkTest.frameEventsTick = s->networkTest.keepAliveTick = 0;
        IHS_SessionChannelControl *control = (void *)s->channels[1];
        IHS_MutexLock(control->sendLock);
        control->sendEncryptSequence = control->recvEncryptSequence = 0;
        IHS_MutexUnlock(control->sendLock);
    }
    IHS_BaseUnlock(&s->base);
}
static bool send_network_stats(IHS_Session *s) {
    float values[5];
    IHS_SessionGetNetworkSample(s, values);
    values[3] = 0;
    CFrameStatsListMsg msg = CFRAME_STATS_LIST_MSG__INIT;
    CFrameStatAccumulatedValue rows[5];
    CFrameStatAccumulatedValue *pointers[5];
    for (unsigned i = 0; i < 5; ++i) {
        cframe_stat_accumulated_value__init(&rows[i]);
        rows[i].stat_type = (EFrameAccumulatedStat)(14 + i);
        rows[i].count = 1;
        rows[i].average = values[i];
        pointers[i] = &rows[i];
    }
    msg.data_type = k_EStreamingVideoData;
    msg.latest_frame_id = 0;
    msg.n_accumulated_stats = 5;
    msg.accumulated_stats = pointers;
    return IHS_SessionChannelStatsSend(s->channels[2], k_EStreamStatsFrameEvents,
                                       (const ProtobufCMessage *)&msg, IHS_PACKET_ID_NEXT);
}
void IHS_SessionNetworkTestUpdate(IHS_Session *s, uint32_t now) {
    IHS_BaseLock(&s->base);
    IHS_SessionNetworkTest *n = &s->networkTest;
    if (n->status != IHS_NetworkTestRunning) {
        IHS_BaseUnlock(&s->base);
        return;
    }
    bool stop = false, complete = false;
    if (!s->mediaNetworkStats.valid) {
        n->phase = 0;
        stop = true;
    } else if (!n->phase || (n->phase == 2 && (int32_t)(n->handshakeTick - now + 655360u) < 0)) {
        n->phase = 0;
        stop = true;
    } else if (n->phase == 5) {
        complete = true;
        n->phase = 0;
        stop = true;
    } else if (n->phase == 4) {
        if (!n->frameEventsTick || (int32_t)(n->frameEventsTick - now + 65536u) < 0) {
            if (!send_network_stats(s)) {
                IHS_BaseUnlock(&s->base);
                IHS_SessionNetworkTestFail(s);
                return;
            }
            n->frameEventsTick = now;
        }
        if ((int32_t)(n->keepAliveTick - now + 65536u) < 0 && !keep_alive(s)) {
            IHS_BaseUnlock(&s->base);
            IHS_SessionNetworkTestFail(s);
            return;
        }
        if ((int32_t)(n->lastData - now + 196608u) < 0)
            n->phase = 5;
    }
    IHS_BaseUnlock(&s->base);
    if (stop) {
        /* Original Complete closes/resets the connection before Stop/scoring. */
        IHS_SessionTransportDisconnect(s);
        IHS_SessionTransportClose(s);
        IHS_BaseLock(&s->base);
        n->lastData = n->handshakeTick = n->frameEventsTick = n->keepAliveTick = 0;
        IHS_SessionChannelControl *control = (void *)s->channels[1];
        IHS_MutexLock(control->sendLock);
        control->sendEncryptSequence = control->recvEncryptSequence = 0;
        IHS_MutexUnlock(control->sendLock);
        if (complete && n->status == IHS_NetworkTestRunning) {
            IHS_MediaNetworkStatsStop(&s->mediaNetworkStats);
            n->loss = 100.0f - (float)s->mediaNetworkStats.total * 100.0f / 600.0f;
            n->result = IHS_MediaNetworkStatsResult(
                s->mediaNetworkStats.average.mean, s->mediaNetworkStats.average.deviation, n->loss);
            n->status =
                s->mediaNetworkStats.valid ? IHS_NetworkTestComplete : IHS_NetworkTestFailed;
        }
        IHS_BaseUnlock(&s->base);
        IHS_SessionInterrupt(s);
    }
}
void IHS_SessionGetNetworkTestResult(IHS_Session *s, IHS_NetworkTestResult *out) {
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    if (!s)
        return;
    IHS_BaseLock(&s->base);
    const IHS_SessionNetworkTest *n = &s->networkTest;
    const IHS_MediaNetworkStats *q = &s->mediaNetworkStats;
    *out = (IHS_NetworkTestResult){.status = n->status,
                                   .mode = n->mode,
                                   .frameSpan = q->total,
                                   .targetKbps = n->currentKbps,
                                   .previousTargetKbps = n->previousKbps,
                                   .lastDataTick = n->lastData,
                                   .samples = q->fullCount,
                                   .progressPercent = (float)q->total * 100.0f / 600.0f,
                                   .lossPercent = n->loss,
                                   .meanNetworkMs = q->average.mean,
                                   .deviationMs = q->average.deviation,
                                   .result = n->result,
                                   .valid = n->mode && q->valid};
    IHS_BaseUnlock(&s->base);
}
size_t IHS_SessionReadNetworkTestSamples(IHS_Session *s, size_t offset, IHS_NetworkTestSample *out,
                                         size_t capacity) {
    if (!s || !out || !capacity)
        return 0;
    if (capacity > 256)
        capacity = 256;
    IHS_BaseLock(&s->base);
    const IHS_MediaNetworkStats *q = &s->mediaNetworkStats;
    size_t count = offset < q->fullCount ? q->fullCount - offset : 0;
    if (count > capacity)
        count = capacity;
    if (count)
        memcpy(out, q->full + offset, count * sizeof(*out));
    IHS_BaseUnlock(&s->base);
    return count;
}
