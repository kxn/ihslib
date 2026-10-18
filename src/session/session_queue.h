/* Private session-owned send/pending queue storage. */
#pragma once
#include "packet.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct IHS_QueueItem {
    IHS_SessionPacket packet;
    bool reliable;
    uint32_t queuedAt;
    uint16_t batchCount; /* reliable frame head: N; continuation: 0 */
    struct IHS_QueueItem *batchNext;
} QueuedPacket;
