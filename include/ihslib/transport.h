#pragma once

#include "common.h"

/** An already-established, message-preserving datagram connection. The upper
 * IHS protocol still owns its encryption, packet IDs, ACK/NACK and retries.
 * In particular, an SNS provider must use unreliable messages, not a reliable
 * byte stream. See nsteamlink STEAM_REMOTE_PLAY_NETWORKING sections 8.9/12.3.
 *
 * The vtable is copied. context remains caller-owned until the session has
 * been interrupted AND joined (or destroyed). No callback destroys context.
 * send may run on multiple IHS threads concurrently with receive; providers
 * must synchronize their own queues. Neither callback may call session APIs.
 */
typedef struct IHS_DatagramTransport {
    /** Copy/submit exactly one whole message before returning. Nonblocking;
     * false means not accepted, so existing IHS retry policy remains in force. */
    bool (*send)(void *context, const uint8_t *data, size_t size);
    /** Return one whole message's size, 0 for no message, -1 for terminal
     * failure. Wait at most timeoutUs, including during shutdown. Never
     * truncate a message: reject oversize messages with -1. The receive buffer
     * is valid only during this callback. Capacity is currently 2048 bytes. */
    int (*receive)(void *context, uint8_t *data, size_t capacity, uint32_t timeoutUs);
} IHS_DatagramTransport;
