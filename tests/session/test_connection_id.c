/* Counter wrap comes from official vectors; this tests its process-wide
 * linearization under concurrent allocation and persistence across IHS_Quit. */
#include "session/session_pri.h"
#include <assert.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
static atomic_uint counts[256];
static void *allocate(void *unused) {
    (void)unused;
    for (unsigned i = 0; i < 10000; ++i)
        atomic_fetch_add(&counts[IHS_SessionNextConnectionId()], 1);
    return NULL;
}
int main(void) {
    IHS_Init();
    unsigned previous = IHS_SessionNextConnectionId();
    assert(previous >= 1 && previous <= 254);
    for (unsigned i = 0; i < 260; ++i) {
        unsigned next = IHS_SessionNextConnectionId();
        assert(next == previous % 255 + 1);
        previous = next;
    }
    pthread_t threads[4];
    for (unsigned i = 0; i < 4; ++i)
        assert(!pthread_create(&threads[i], NULL, allocate, NULL));
    for (unsigned i = 0; i < 4; ++i)
        assert(!pthread_join(threads[i], NULL));
    assert(!atomic_load(&counts[0]));
    unsigned min = UINT_MAX, max = 0, total = 0;
    for (unsigned i = 1; i < 256; ++i) {
        unsigned value = atomic_load(&counts[i]);
        if (value < min)
            min = value;
        if (value > max)
            max = value;
        total += value;
    }
    assert(total == 40000 && max - min <= 1);
    IHS_Quit();
    IHS_Init();
    assert(IHS_SessionNextConnectionId() == (previous + 40000) % 255 + 1);
    IHS_Quit();
    return 0;
}
