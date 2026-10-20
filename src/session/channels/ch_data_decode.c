/* Media consumer boundaries, separate from connection/FIFO ownership. */
#include "ch_data.h"
#include "session/session_pri.h"
#include <assert.h>

void IHS_SessionChannelDataReceiveFrame(IHS_SessionChannelData *channel, IHS_SessionFrame *frame) {
    assert(frame->header.type == IHS_SessionPacketTypeUnreliable ||
           frame->header.type == IHS_SessionPacketTypeUnreliableFEC ||
           frame->header.type == IHS_SessionPacketTypeReliable);
    if (frame->body.size < 1)
        return;
    EStreamDataMessage type = *IHS_BufferPointer(&frame->body);
    IHS_BufferOffsetBy(&frame->body, 1);
    if (type != k_EStreamDataPacket) {
        return;
    }
    IHS_SessionDataFrameHeader header = {0};
    bool hasHeader = false;
    if (frame->body.size >= IHS_SESSION_DATA_FRAME_HEADER_SIZE) {
        hasHeader = true;
        size_t offset = IHS_SessionChannelDataFrameHeaderParse(&header, &frame->body);
        IHS_BufferOffsetBy(&frame->body, (int)offset);
        header.sendTimestamp = frame->header.sendTimestamp;
        header.receiveTimestamp = frame->header.receiveTimestamp;
        header.receiveBegin = frame->header.receiveBegin;
        if (frame->mediaPrepared) {
            header.timestamp -= frame->mediaOffset;
            header.sendTimestamp -= frame->mediaOffset;
            if (header.inputRecvTimestamp)
                header.inputRecvTimestamp -= frame->mediaOffset;
            header.timestampsLocal = true;
            header.receivedRecorded = true;
        }
    }
    const IHS_SessionChannelDataClass *cls = (const IHS_SessionChannelDataClass *)channel->base.cls;
    if (hasHeader)
        cls->dataFrame((IHS_SessionChannel *)channel, &header, &frame->body);
}

uint32_t IHS_SessionChannelDataThink(IHS_SessionChannel *channel) {
    const IHS_SessionChannelDataClass *cls = (const void *)channel->cls;
    return cls->think ? cls->think(channel) : UINT32_MAX;
}
