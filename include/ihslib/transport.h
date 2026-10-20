#pragma once

#include "common.h"

/** A message-preserving datagram connection. A provider may still be finding
 * its route: reject sends for the existing retry policy and return empty reads
 * until data arrives, reserving terminal failure for a failed/closed provider.
 * The upper IHS protocol still owns its encryption, packet IDs, ACK/NACK and retries.
 * In particular, an SNS provider must use unreliable messages, not a reliable
 * byte stream. See nsteamlink STEAM_REMOTE_PLAY_NETWORKING sections 8.9/12.3.
 *
 * The vtable is copied. context remains caller-owned until the session has
 * been interrupted AND joined (or destroyed). No callback destroys context.
 * send may run on multiple IHS threads concurrently with receive; providers
 * must synchronize their own queues. No callback may call session APIs.
 */
typedef struct IHS_DatagramTransport {
    /** Copy/submit exactly one whole message before returning. Nonblocking;
     * false means not accepted, so existing IHS retry policy remains in force. */
    bool (*send)(void *context, const uint8_t *data, size_t size);
    /** Receive one whole message: 1 success (including an empty message),
     * 0 empty queue, -1 terminal failure. On success set *received. Wait at
     * most timeoutUs, including during shutdown; the session drains with
     * timeoutUs=0 after wait reports readability. Never truncate: oversize
     * messages must fail. The buffer is valid only during this callback. */
    int (*receive)(void *context, uint8_t *data, size_t capacity, size_t *received,
                   uint32_t timeoutUs);
    /** Wait without consuming a message: 1 readable, 0 empty, -1 terminal.
     * This separates waiting from the captured receive-batch timestamp. */
    int (*wait)(void *context, uint32_t timeoutUs);
    /** Provider's maximum whole message, independent of ordinary UDP's
     * receive buffer and of negotiated MTU. Zero uses the stream default. */
    size_t receiveCapacity;
    /** Optional nonconsuming size hint for the next queued message. Lets an
     * SDK provider retain full-size messages without allocating its maximum
     * for every small packet; must not exceed receiveCapacity. A zero-length
     * message is valid. receive still reports empty/terminal races. */
    size_t (*nextMessageSize)(void *context);
} IHS_DatagramTransport;
