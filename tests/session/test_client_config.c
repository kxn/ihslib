/* Config ownership/reentrancy and concurrent readers, independent of the
 * fixed original-instruction oracle. No network or display is exercised. */
#include "ihs_buffer.h"
#include "ihs_buffer_ext.h"
#include "session/channels/ch_control.h"
#include "session/session_pri.h"
#include "test_session.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    IHS_SessionPacket packet;
    bool reliable;
} QueuedPacket;
typedef struct {
    bool reenter, entered;
    char order[16];
    unsigned index;
    uint8_t *saved;
    size_t savedSize;
} Delegate;
static const uint8_t disabled[] = {0x58, 0x00};
static const uint8_t fec[] = {0xc8, 0x01, 0x01};
static void changed(IHS_Session *s, const uint8_t *wire, size_t size, bool local, void *ctx) {
    (void)wire;
    (void)size;
    (void)local;
    Delegate *d = ctx;
    d->order[d->index++] = 'N';
    if (d->reenter && !d->entered) {
        d->entered = true;
        assert(!IHS_SessionInputEnabled(s));
        assert(IHS_SessionSetStreamingClientConfig(s, fec, sizeof(fec), true));
    }
}
static void save(IHS_Session *s, const uint8_t *wire, size_t size, void *ctx) {
    Delegate *d = ctx;
    d->order[d->index++] = 'S';
    free(d->saved);
    d->saved = malloc(size + 1);
    assert(d->saved);
    memcpy(d->saved, wire, size);
    d->savedSize = size;
    uint8_t current[128];
    assert(IHS_SessionGetStreamingClientConfig(s, current, sizeof(current)) == size);
    assert(!memcmp(current, wire, size));
}
static IHS_Session *create(void) {
    IHS_Session *s = IHS_TestSessionCreate();
    assert(s);
    IHS_TimerTaskStopImmediate(s->retransmission.timer);
    return s;
}
static void reentrant(void) {
    IHS_Session *s = create();
    Delegate d = {.reenter = true};
    const IHS_StreamSessionCallbacks callbacks = {.clientConfigChanged = changed,
                                                  .saveClientConfig = save};
    IHS_SessionSetSessionCallbacks(s, &callbacks, &d);
    s->state.connectionState = IHS_SessionConnectionStateConnected;
    assert(IHS_SessionSetStreamingClientConfig(s, disabled, sizeof(disabled), false));
    assert(d.index == 4 && !memcmp(d.order, "NNSS", 4));
    assert(IHS_SessionInputEnabled(s));
    CStreamingClientConfig *current = cstreaming_client_config__unpack(NULL, d.savedSize, d.saved);
    assert(current && current->enable_unreliable_fec && current->enable_input_streaming);
    cstreaming_client_config__free_unpacked(current, NULL);
    // The outer API input is independent, so its transmitted config is still
    // input=false (the final saved/local value reflects the recursive delegate).
    QueuedPacket *q = (void *)IHS_QueuePoll(s->sendQueue);
    assert(q);
    assert(*IHS_BufferPointer(&q->packet.body) == 99);
    IHS_BufferOffsetBy(&q->packet.body, 1);
    IHS_Buffer plain = IHS_BUFFER_INIT(128, 0);
    uint64_t seq;
    assert(IHS_SessionFrameDecrypt(s, &q->packet.body, &plain, 0, &seq) ==
           IHS_SessionFrameDecryptOK);
    CSetStreamingClientConfig *message =
        cset_streaming_client_config__unpack(NULL, plain.size, IHS_BufferPointer(&plain));
    assert(message && message->config && !message->config->enable_input_streaming &&
           !message->config->enable_unreliable_fec);
    cset_streaming_client_config__free_unpacked(message, NULL);
    IHS_BufferClear(&plain, true);
    IHS_SessionPacketClear(&q->packet, true);
    IHS_QueueItemFree((void *)q);
    assert(!IHS_QueuePoll(s->sendQueue));
    // Insufficient destination capacity is a size query and must not write.
    uint8_t shortBuffer[2] = {0x5a, 0xa5};
    assert(IHS_SessionGetStreamingClientConfig(s, shortBuffer, 1) == sizeof(fec));
    assert(shortBuffer[0] == 0x5a && shortBuffer[1] == 0xa5);
    IHS_SessionDestroy(s);
    // The application saved an owned copy, valid after the session is gone.
    assert(d.savedSize == sizeof(fec) && !memcmp(d.saved, fec, sizeof(fec)));
    free(d.saved);
}
typedef struct {
    IHS_Session *session;
    atomic_bool start, done;
    atomic_uint reads;
} Concurrent;
static void *reader(void *ctx) {
    Concurrent *c = ctx;
    atomic_store(&c->start, true);
    do {
        uint8_t wire[128];
        size_t n = IHS_SessionGetStreamingClientConfig(c->session, wire, sizeof(wire));
        assert(n <= sizeof(wire));
        CStreamingClientConfig *config = cstreaming_client_config__unpack(NULL, n, wire);
        assert(config);
        assert((config->enable_unreliable_fec && config->enable_input_streaming) ||
               (!config->enable_unreliable_fec && !config->enable_input_streaming));
        cstreaming_client_config__free_unpacked(config, NULL);
        (void)IHS_SessionInputEnabled(c->session);
        atomic_fetch_add(&c->reads, 1);
    } while (!atomic_load(&c->done));
    return NULL;
}
static void concurrent(void) {
    IHS_Session *s = create();
    assert(IHS_SessionSetStreamingClientConfig(s, disabled, sizeof(disabled), true));
    Concurrent c = {.session = s};
    atomic_init(&c.start, false);
    atomic_init(&c.done, false);
    atomic_init(&c.reads, 0);
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, reader, &c));
    while (!atomic_load(&c.start)) {
    }
    for (unsigned i = 0; i < 4000; ++i) {
        const uint8_t *wire = i & 1 ? fec : disabled;
        assert(IHS_SessionSetStreamingClientConfig(s, wire, i & 1 ? sizeof(fec) : sizeof(disabled),
                                                   true));
    }
    atomic_store(&c.done, true);
    assert(!pthread_join(thread, NULL));
    assert(atomic_load(&c.reads) > 0);
    IHS_SessionDestroy(s);
}
int main(void) {
    IHS_Init();
    reentrant();
    concurrent();
    IHS_Quit();
    return 0;
}
