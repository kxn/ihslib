/* Observe the production POSIX primitives at OS boundaries. The Python driver
 * compares deadlines to official event output and checks cleanup/error paths.
 * Wall-clock changes here are controlled clock inputs, not privileged OS edits. */
#define _GNU_SOURCE
#include "ihs_thread.h"
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static bool observe, simulate;
static int fault, attrInits, attrDestroys, condInits, condDestroys;
static int attrClock = -1, readClock = -1, waits, waitResult, clockError, realWaits;
static struct timespec now, deadline;
int __real_pthread_condattr_init(pthread_condattr_t *);
int __real_pthread_condattr_setclock(pthread_condattr_t *, clockid_t);
int __real_pthread_condattr_destroy(pthread_condattr_t *);
int __real_pthread_cond_init(pthread_cond_t *, const pthread_condattr_t *);
int __real_pthread_cond_destroy(pthread_cond_t *);
int __real_clock_gettime(clockid_t, struct timespec *);
int __real_pthread_cond_timedwait(pthread_cond_t *, pthread_mutex_t *, const struct timespec *);
int __wrap_pthread_condattr_init(pthread_condattr_t *a) {
    if (observe) {
        ++attrInits;
        if (fault == 1)
            return EINVAL;
    }
    return __real_pthread_condattr_init(a);
}
int __wrap_pthread_condattr_setclock(pthread_condattr_t *a, clockid_t c) {
    if (observe && fault == 2)
        return EINVAL;
    return __real_pthread_condattr_setclock(a, c);
}
int __wrap_pthread_condattr_destroy(pthread_condattr_t *a) {
    if (observe)
        ++attrDestroys;
    return __real_pthread_condattr_destroy(a);
}
int __wrap_pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *a) {
    if (observe) {
        ++condInits;
        clockid_t id = CLOCK_REALTIME;
        if (a)
            assert(pthread_condattr_getclock(a, &id) == 0);
        attrClock = id;
        if (fault == 3)
            return EINVAL;
    }
    return __real_pthread_cond_init(c, a);
}
int __wrap_pthread_cond_destroy(pthread_cond_t *c) {
    if (observe)
        ++condDestroys;
    return __real_pthread_cond_destroy(c);
}
int __wrap_clock_gettime(clockid_t c, struct timespec *t) {
    if (observe) {
        readClock = c;
        if (simulate) {
            if (clockError) {
                errno = EIO;
                return -1;
            }
            *t = now;
            /* A wall clock which has moved an hour in either direction. */
            if (c == CLOCK_REALTIME)
                t->tv_sec += fault == 5 ? -3600 : 3600;
            return 0;
        }
    }
    return __real_clock_gettime(c, t);
}
int __wrap_pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *t) {
    if (observe) {
        ++waits;
        deadline = *t;
        if (simulate)
            return waitResult;
    }
    /* Bound the fixture even if a regression pairs an epoch deadline with a
     * monotonic condvar. Such a call could otherwise sleep for decades. */
    if (observe && (readClock != attrClock || t->tv_nsec >= 1000000000))
        return EINVAL;
    ++realWaits;
    return __real_pthread_cond_timedwait(c, m, t);
}
static void reset(void) {
    fault = attrInits = attrDestroys = condInits = condDestroys = waits = clockError = 0;
    attrClock = readClock = -1;
    deadline = (struct timespec){0};
}
int main(int argc, char **argv) {
    observe = true;
    simulate = true;
    printf("{\"monotonic\":%d,\"realtime\":%d,\"creation\":[", CLOCK_MONOTONIC, CLOCK_REALTIME);
    for (int f = 0; f < 4; ++f) {
        reset();
        fault = f;
        IHS_Cond *c = IHS_CondCreate();
        bool created = c != NULL;
        IHS_CondDestroy(c);
        printf("%s[%d,%d,%d,%d,%d,%d,%d]", f ? "," : "", f, created, attrInits, attrDestroys,
               condInits, condDestroys, attrClock);
    }
    reset();
    IHS_Cond *c = IHS_CondCreate();
    IHS_Mutex *mutex = IHS_MutexCreate();
    assert(c && mutex && IHS_MutexLock(mutex));
    printf("],\"waits\":[");
    const uint32_t durations[] = {0, 1, 16, 1000, UINT32_MAX - 1};
    bool comma = false;
    for (int wall = 4; wall <= 5; ++wall) {
        for (size_t i = 0; i < sizeof(durations) / sizeof(*durations); ++i) {
            for (int r = 0; r < 4; ++r) {
                fault = wall;
                readClock = -1;
                waits = 0;
                now = (struct timespec){15, 999500000};
                const int results[] = {0, EINTR, ETIMEDOUT, EINVAL};
                waitResult = results[r];
                bool success = IHS_CondWaitTimeout(c, mutex, durations[i]);
                printf("%s[%d,%u,%d,%d,%d,%d,%" PRIdMAX ",%ld]", comma ? "," : "", wall,
                       durations[i], r, success, readClock, waits, (intmax_t)deadline.tv_sec,
                       deadline.tv_nsec);
                comma = true;
            }
        }
    }
    clockError = 1;
    waits = 0;
    bool failedClock = IHS_CondWaitTimeout(c, mutex, 1);
    printf("],\"clock_error\":[%d,%d]", failedClock, waits);
    clockError = 0;
    simulate = false;
    struct timespec before, after;
    assert(__real_clock_gettime(CLOCK_MONOTONIC, &before) == 0);
    bool timedOut = !IHS_CondWaitTimeout(c, mutex, 2);
    assert(__real_clock_gettime(CLOCK_MONOTONIC, &after) == 0);
    int64_t elapsed =
        (int64_t)(after.tv_sec - before.tv_sec) * 1000000000 + after.tv_nsec - before.tv_nsec;
    assert(IHS_MutexUnlock(mutex));
    IHS_CondDestroy(c);
    IHS_MutexDestroy(mutex);
    printf(",\"real_timeout\":[%d,%d,%d],\"elapsed_ns\":%" PRId64 "}\n", timedOut, readClock,
           elapsed >= 1000000 && realWaits == 1, elapsed);
    (void)argc;
    (void)argv;
    return 0;
}
