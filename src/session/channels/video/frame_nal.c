#include "frame_nal.h"
#include "ihs_buffer_ext.h"
#include <stdint.h>
#include <string.h>

bool IHS_SessionVideoFrameAppendNAL(IHS_Buffer *buffer, const uint8_t *data, size_t len,
                                    const IHS_VideoFrameHeader *header) {
    if (!(header->flags & VideoFrameFlagNeedEscape)) {
        if (len) IHS_BufferAppendMem(buffer, data, len);
        return true;
    }
    bool start = (header->flags & VideoFrameFlagNeedStartSequence) != 0;
    if (start && !len) return false; /* Original AppendBuffer assert60. */
    /* Source's continuation helper reads destination[-2/-1]. Do not infer a
     * zero history from allocator memory. One known nonzero suffix byte is
     * sufficient to rule out insertion without reading a missing byte. */
    if (!start && len && data[0] <= 3 &&
        (!buffer->size || (buffer->size == 1 && IHS_BufferPointer(buffer)[0] == 0)))
        return false;
    if (len > (SIZE_MAX - 5) / 3 * 2) return false;
    size_t reserve = len + len / 2 + 5;
    if (buffer->maxCapacity && (buffer->size > buffer->maxCapacity ||
        reserve > buffer->maxCapacity - buffer->size))
        return false;
    uint8_t *base = IHS_BufferPointerForAppend(buffer, reserve);
    uint8_t *dst = base;
    size_t history = buffer->size;
    if (start) {
        const uint8_t prefix[] = {0, 0, 0, 1};
        memcpy(dst, prefix, sizeof(prefix));
        dst += sizeof(prefix);
        size_t direct = len < 3 ? len : 3;
        memcpy(dst, data, direct);
        dst += direct;
        data += direct;
        len -= direct;
        history += sizeof(prefix) + direct;
    }
    for (size_t n = 0; n < len; ++n) {
        uint8_t value = data[n];
        if (value <= 3 && history >= 2 && !dst[-2] && !dst[-1]) {
            *dst++ = 3;
            ++history;
        }
        *dst++ = value;
        ++history;
    }
    buffer->size += (size_t)(dst - base);
    return true;
}
