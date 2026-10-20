/* Wire coefficients from official Android 1.3.32 AddPacket 0x7f7c10:
 * parity[row][col] = GF256_inverse[(m + col) XOR row], polynomial 0x11d.
 * No external RS library or copied executable tables are required. */
#include "fec.h"
#include "endianness.h"
#include <stdlib.h>
#include <string.h>

struct IHS_FECReceiver {
    uint8_t multiply[256][256], inverse[256];
    uint8_t *shards, *present;
    uint32_t length, dataCount;
    uint16_t shardSize, parityCount, group, lastDelivered;
    uint32_t received;
    IHS_SessionPacketHeader firstHeader;
    IHS_FECReceiveError error;
};

IHS_FECReceiver *IHS_FECReceiverCreate(void) {
    IHS_FECReceiver *r = calloc(1, sizeof(*r));
    if (!r)
        return NULL;
    uint8_t exp[510], log[256] = {0};
    unsigned value = 1;
    for (unsigned i = 0; i < 255; ++i) {
        exp[i] = exp[i + 255] = (uint8_t)value;
        log[value] = (uint8_t)i;
        value <<= 1;
        if (value & 256)
            value ^= 0x11d;
    }
    for (unsigned a = 1; a < 256; ++a) {
        r->inverse[a] = exp[255 - log[a]];
        for (unsigned b = 1; b < 256; ++b)
            r->multiply[a][b] = exp[log[a] + log[b]];
    }
    r->lastDelivered = UINT16_MAX; /* CFECIncomingQueue constructor 0x7f86cc */
    return r;
}

static void Reset(IHS_FECReceiver *r) {
    free(r->shards);
    free(r->present);
    r->shards = r->present = NULL;
    r->received = 0;
}

void IHS_FECReceiverDestroy(IHS_FECReceiver *r) {
    if (!r)
        return;
    Reset(r);
    free(r);
}

IHS_FECReceiveError IHS_FECReceiverLastError(const IHS_FECReceiver *r) {
    return r->error;
}

static void MultiplyAdd(const IHS_FECReceiver *r, uint8_t *dst, const uint8_t *src,
                        uint8_t coefficient, size_t length) {
    const uint8_t *table = r->multiply[coefficient];
    for (size_t i = 0; i < length; ++i)
        dst[i] ^= table[src[i]];
}

/* Solve only the missing data columns. Received systematic columns are known;
 * subtract them from parity, then invert the remaining square Cauchy matrix.
 * This has the same codewords as the original full-matrix RS decoder. */
static bool Reconstruct(IHS_FECReceiver *r) {
    uint16_t missing[255], parity[255];
    unsigned count = 0, available = 0;
    for (unsigned i = 0; i < r->dataCount; ++i) {
        if (!r->present[i]) {
            if (count == 255)
                return false;
            missing[count++] = (uint16_t)i;
        }
    }
    if (!count)
        return true;
    if (!r->parityCount || count > r->parityCount || (unsigned)r->dataCount + r->parityCount > 255)
        return false;
    for (unsigned i = 0; i < r->parityCount && available < count; ++i)
        if (r->present[r->dataCount + i])
            parity[available++] = (uint16_t)i;
    if (available != count)
        return false;
    size_t width = count * 2u;
    uint8_t *matrix = calloc(count, width);
    uint8_t *rhs = calloc(count, r->shardSize);
    if (!matrix || !rhs) {
        free(matrix);
        free(rhs);
        r->error = IHS_FECReceiveNoMemory;
        return false;
    }
    for (unsigned row = 0; row < count; ++row) {
        uint8_t *equation = matrix + row * width;
        for (unsigned col = 0; col < count; ++col)
            equation[col] = r->inverse[(r->parityCount + missing[col]) ^ parity[row]];
        equation[count + row] = 1;
        uint8_t *bytes = rhs + (size_t)row * r->shardSize;
        memcpy(bytes, r->shards + (size_t)(r->dataCount + parity[row]) * r->shardSize,
               r->shardSize);
        for (unsigned col = 0; col < r->dataCount; ++col)
            if (r->present[col])
                MultiplyAdd(r, bytes, r->shards + (size_t)col * r->shardSize,
                            r->inverse[(r->parityCount + col) ^ parity[row]], r->shardSize);
    }
    bool ok = true;
    for (unsigned col = 0; col < count; ++col) {
        unsigned pivot = col;
        while (pivot < count && !matrix[pivot * width + col])
            ++pivot;
        if (pivot == count) {
            ok = false;
            break;
        }
        uint8_t *row = matrix + col * width;
        if (pivot != col)
            for (size_t i = 0; i < width; ++i) {
                uint8_t tmp = row[i];
                row[i] = matrix[pivot * width + i];
                matrix[pivot * width + i] = tmp;
            }
        uint8_t scale = r->inverse[row[col]];
        for (size_t i = 0; i < width; ++i)
            row[i] = r->multiply[scale][row[i]];
        for (unsigned other = 0; other < count; ++other) {
            if (other == col)
                continue;
            uint8_t *target = matrix + other * width;
            uint8_t factor = target[col];
            if (factor)
                MultiplyAdd(r, target, row, factor, width);
        }
    }
    if (ok)
        for (unsigned row = 0; row < count; ++row) {
            uint8_t *dst = r->shards + (size_t)missing[row] * r->shardSize;
            memset(dst, 0, r->shardSize);
            for (unsigned col = 0; col < count; ++col)
                MultiplyAdd(r, dst, rhs + (size_t)col * r->shardSize,
                            matrix[row * width + count + col], r->shardSize);
        }
    free(matrix);
    free(rhs);
    return ok;
}

bool IHS_FECReceiverAdd(IHS_FECReceiver *r, const IHS_SessionPacket *packet,
                        IHS_SessionFrame *frame) {
    if (!r)
        return false;
    r->error = IHS_FECReceiveOK;
    if (packet->header.type != IHS_SessionPacketTypeUnreliableFEC || packet->body.size < 9 ||
        !packet->body.data)
        return false;
    uint16_t group = packet->header.packetId;
    uint16_t distance = (uint16_t)(group - r->lastDelivered);
    if (!distance || distance >= 0x8000)
        return false;
    const uint8_t *bytes = IHS_BufferPointer(&packet->body);
    uint32_t length;
    uint16_t shardSize, parity;
    IHS_ReadUInt32LE(bytes, &length);
    IHS_ReadUInt16LE(bytes + 4, &shardSize);
    IHS_ReadUInt16LE(bytes + 6, &parity);
    if (!length || length > IHS_FEC_MAX_DATA_BYTES || !shardSize ||
        packet->body.size != (size_t)shardSize + 8)
        return false;
    uint32_t data = (length + shardSize - 1u) / shardSize;
    uint32_t total = data + parity;
    uint16_t index = (uint16_t)packet->header.fragmentId;
    /* Coding has at most 255 GF elements. No-parity groups may be larger;
     * the 16-bit shard index and payload bound still limit their allocation. */
    if ((size_t)data * shardSize > IHS_FEC_MAX_DATA_BYTES || total > (uint32_t)UINT16_MAX + 1u ||
        index >= total || (parity && total > 255))
        return false;
    if (r->shards && r->group == group &&
        (r->length != length || r->shardSize != shardSize || r->parityCount != parity))
        return false; /* hardening: inconsistent metadata cannot resize an active group */
    if (!r->shards || r->group != group) {
        Reset(r); /* matches the original single-active-group policy, including rollback */
        r->shards = calloc(total, shardSize);
        r->present = calloc(total, 1);
        if (!r->shards || !r->present) {
            Reset(r);
            r->error = IHS_FECReceiveNoMemory;
            return false;
        }
        r->group = group;
        r->length = length;
        r->shardSize = shardSize;
        r->dataCount = data;
        r->parityCount = parity;
        r->firstHeader = packet->header;
        r->firstHeader.receiveBegin = 0;
    }
    /* BAddPacket 0x7f82a0..82dc: choose the lowest wire index, while
     * first/last receive timestamps have independent sentinel/arrival rules. */
    uint32_t begin = r->firstHeader.receiveBegin;
    if (index < (uint16_t)r->firstHeader.fragmentId)
        r->firstHeader = packet->header;
    r->firstHeader.receiveBegin = begin ? begin : packet->header.receiveTimestamp;
    r->firstHeader.receiveTimestamp = packet->header.receiveTimestamp;
    if (r->present[index])
        return false; /* original duplicate-count delivery bug is deliberately not copied */
    memcpy(r->shards + (size_t)index * shardSize, bytes + 8, shardSize);
    r->present[index] = 1;
    ++r->received;
    if (r->received < data || !Reconstruct(r))
        return false;
    *frame = (IHS_SessionFrame){.header = r->firstHeader,
                                .body = {.data = r->shards,
                                         .size = length,
                                         .capacity = (size_t)total * shardSize,
                                         .maxCapacity = (size_t)total * shardSize}};
    r->shards = NULL;
    r->lastDelivered = group;
    Reset(r);
    return true;
}
