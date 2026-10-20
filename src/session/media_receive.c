/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Official OnDataPacket 7ad220..7ad298: normalize and sample before decoder.
 * Decoder FIFO delay must not change the offset applied to an accepted frame. */
#include "channels/ch_data.h"
#include "frame_stats.h"
#include "session_pri.h"

void IHS_SessionChannelDataPrepareFrame(IHS_SessionChannel *channel, IHS_SessionFrame *frame) {
    if (frame->mediaPrepared || frame->body.size < 1 + IHS_SESSION_DATA_FRAME_HEADER_SIZE)
        return;
    IHS_Buffer body = frame->body;
    if (*IHS_BufferPointer(&body) != k_EStreamDataPacket)
        return;
    IHS_BufferOffsetBy(&body, 1);
    IHS_SessionDataFrameHeader h = {0};
    if (!IHS_SessionChannelDataFrameHeaderParse(&h, &body))
        return;
    frame->mediaOffset = IHS_StreamClockOffset(&channel->session->clock);
    frame->mediaPrepared = true;
    uint32_t sent = frame->header.sendTimestamp - frame->mediaOffset;
    uint32_t input = h.inputRecvTimestamp ? h.inputRecvTimestamp - frame->mediaOffset : 0;
    uint32_t size = (uint32_t)(body.size - IHS_SESSION_DATA_FRAME_HEADER_SIZE);
    if (channel->type == IHS_SessionChannelTypeDataVideo) {
        IHS_BaseLock(&channel->session->base);
        IHS_MediaNetworkStatsRecord(&channel->session->mediaNetworkStats, h.id, sent,
                                     frame->header.receiveTimestamp);
        IHS_BaseUnlock(&channel->session->base);
        IHS_SessionRecordNetworkVideo(channel->session, sent, frame->header.receiveBegin,
                                      frame->header.receiveTimestamp, h.id, size);
    }
    if (channel->session->frameStats)
        IHS_FrameStatsRecordMediaReceived(channel->session->frameStats,
                                          channel->type == IHS_SessionChannelTypeDataAudio
                                              ? IHS_FrameStatsMediaAudio
                                              : IHS_FrameStatsMediaVideo,
                                          h.id, h.timestamp - frame->mediaOffset, sent,
                                          frame->header.receiveTimestamp, size, h.inputMark, input);
}
