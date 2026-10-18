#pragma once
#include "ch_data_video.h"
/* Original H264-family byte assembly. False is a bounded local precondition
 * failure, not an additional peer recovery policy. */
bool IHS_SessionVideoFrameAppendNAL(IHS_Buffer *, const uint8_t *, size_t,
                                    const IHS_VideoFrameHeader *);
