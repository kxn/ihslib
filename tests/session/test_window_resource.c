/* Resource errors are local outcomes, not missing-packet feedback. The
 * failing realloc and impossible descriptors are explicit test adapters. */
#include "ihs_buffer.h"
#include "session/window.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
static bool fail_next;
static unsigned failed_allocations;
void *__real_realloc(void *, size_t);
void *__wrap_realloc(void *p, size_t n) {
    if (fail_next) {
        fail_next = false;
        ++failed_allocations;
        return NULL;
    }
    return __real_realloc(p, n);
}
static IHS_SessionPacketsWindow *one(void) {
    IHS_SessionPacketsWindow *w = IHS_SessionPacketsWindowCreateMedia(8);
    assert(w);
    uint8_t *bytes = malloc(24);
    assert(bytes);
    memset(bytes, 0xa5, 24);
    IHS_SessionPacket p = {.header = {.type = IHS_SessionPacketTypeUnreliable, .packetId = 0},
                           .body = IHS_BUFFER_WRAP(bytes, 24)};
    assert(IHS_SessionPacketsWindowAdd(w, &p));
    IHS_SessionPacketClear(&p, true);
    return w;
}
static void limits_and_retry(void) {
    IHS_SessionPacketsWindow *w = one();
    IHS_SessionFrame f = {.header = {.packetId = 12345}, .body = IHS_BUFFER_INIT(8, 16)};
    IHS_BufferEnsureMaxSize(&f.body, 8);
    memset(f.body.data, 0x5a, 8);
    f.body.size = 5;
    IHS_SessionFrame before = f;
    assert(!IHS_SessionPacketsWindowPoll(w, &f));
    assert(IHS_SessionPacketsWindowLastError(w) == IHS_WindowPollLimit);
    assert(!memcmp(&f, &before, sizeof(f)) && IHS_SessionPacketsWindowSize(w) == 1);
    f.body.maxCapacity = 24; /* Valid non-power-of-two caller bound. */
    assert(IHS_SessionPacketsWindowPoll(w, &f));
    assert(IHS_SessionPacketsWindowLastError(w) == IHS_WindowPollOK && f.body.size == 24 &&
           f.body.capacity == 24);
    for (size_t i = 0; i < 24; ++i)
        assert(f.body.data[i] == 0xa5);
    assert(!IHS_SessionPacketsWindowPoll(w, &f) &&
           IHS_SessionPacketsWindowLastError(w) == IHS_WindowPollOK);
    IHS_BufferClear(&f.body, true);
    IHS_SessionPacketsWindowDestroy(w);
}
static void allocation_failure(void) {
    IHS_SessionPacketsWindow *w = one();
    IHS_SessionFrame f = {.header = {.packetId = 12345}, .body = IHS_BUFFER_INIT(8, 0)};
    IHS_BufferEnsureMaxSize(&f.body, 8);
    memset(f.body.data, 0x5a, 8);
    f.body.size = 5;
    IHS_SessionFrame before = f;
    fail_next = true;
    assert(!IHS_SessionPacketsWindowPoll(w, &f) && !fail_next && failed_allocations == 1);
    assert(IHS_SessionPacketsWindowLastError(w) == IHS_WindowPollNoMemory);
    assert(!memcmp(&f, &before, sizeof(f)) && IHS_SessionPacketsWindowSize(w) == 1);
    for (size_t i = 0; i < 8; ++i)
        assert(f.body.data[i] == 0x5a);
    assert(IHS_SessionPacketsWindowPoll(w, &f) && f.body.size == 24);
    IHS_BufferClear(&f.body, true);
    IHS_SessionPacketsWindowDestroy(w);
}
static void overflow_descriptors(void) {
    IHS_SessionPacketsWindow *w = one();
    IHS_SessionFrame f = {.body = {.offset = SIZE_MAX, .suffix = 1}};
    assert(!IHS_SessionPacketsWindowPoll(w, &f));
    assert(IHS_SessionPacketsWindowLastError(w) == IHS_WindowPollLimit && !f.body.data);
    IHS_SessionPacketsWindowDestroy(w);
    w = IHS_SessionPacketsWindowCreateMedia(8);
    assert(w);
    for (unsigned i = 0; i < 2; ++i) {
        uint8_t *p = malloc(1);
        assert(p);
        IHS_SessionPacket packet = {
            .header = {.type = i ? IHS_SessionPacketTypeUnreliableFrag
                                 : IHS_SessionPacketTypeUnreliable,
                       .packetId = i,
                       .fragmentId = i ? 0 : 1},
            .body = {.data = p, .size = i ? 16 : SIZE_MAX - 4, .capacity = SIZE_MAX}};
        assert(IHS_SessionPacketsWindowAdd(w, &packet));
        IHS_SessionPacketClear(&packet, true);
    }
    f = (IHS_SessionFrame){.body = IHS_BUFFER_INIT(8, 0)};
    assert(!IHS_SessionPacketsWindowPoll(w, &f));
    assert(IHS_SessionPacketsWindowLastError(w) == IHS_WindowPollLimit && !f.body.data);
    IHS_SessionPacketsWindowDestroy(w);
}
static void zero_body(bool bounded) {
    IHS_SessionPacketsWindow *w = IHS_SessionPacketsWindowCreateMedia(8);
    assert(w);
    uint8_t *p = malloc(1);
    assert(p);
    IHS_SessionPacket packet = {.header = {.type = IHS_SessionPacketTypeUnreliable},
                                .body = {.data = p, .capacity = 1}};
    assert(IHS_SessionPacketsWindowAdd(w, &packet));
    IHS_SessionPacketClear(&packet, true);
    IHS_SessionFrame f = {.body = bounded ? (IHS_Buffer)IHS_BUFFER_INIT(64, 16)
                                          : (IHS_Buffer)IHS_BUFFER_INIT(8, 0)};
    assert(IHS_SessionPacketsWindowPoll(w, &f) && f.body.data && !f.body.size);
    assert(f.body.capacity == (bounded ? 16 : 8));
    IHS_BufferClear(&f.body, true);
    IHS_SessionPacketsWindowDestroy(w);
}
int main(void) {
    limits_and_retry();
    allocation_failure();
    overflow_descriptors();
    zero_body(false);
    zero_body(true);
    return 0;
}
