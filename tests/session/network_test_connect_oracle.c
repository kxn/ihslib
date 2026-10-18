/* Actual public Connect and RX/TX owners. Clock/final send are explicit
 * adapters for the original dedicated-test BConnect vectors (§123). */
#include "ihs_buffer.h"
#include "ihslib.h"
#include "session/session_pri.h"
#include "session/unreliable.h"
#include "test_session.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint32_t stamp;
static int send_result;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned size;
static uint8_t first[64];
static bool fail_receiver;
IHS_UnreliableChannel *__real_IHS_SessionUnreliableGet(IHS_Session *, uint8_t, uint16_t);
IHS_UnreliableChannel *__wrap_IHS_SessionUnreliableGet(IHS_Session *s, uint8_t id, uint16_t n) {
    return fail_receiver ? NULL : __real_IHS_SessionUnreliableGet(s, id, n);
}
uint32_t __wrap_IHS_SessionPacketTimestamp(void) {
    return stamp;
}
bool __wrap_IHS_BaseSend(IHS_Base *base, IHS_SocketAddress address, const IHS_Buffer *wire) {
    (void)base;
    (void)address;
    if (wire->size && (IHS_BufferPointer(wire)[0] & 127) == 1) {
        pthread_mutex_lock(&lock);
        if (!size) {
            assert(wire->size <= sizeof(first));
            size = wire->size;
            memcpy(first, IHS_BufferPointer(wire), size);
        }
        pthread_mutex_unlock(&lock);
    }
    return send_result == 0;
}
int main(int argc, char **argv) {
    assert(argc == 4 || argc == 5);
    unsigned mode = strtoul(argv[1], NULL, 0);
    stamp = strtoul(argv[2], NULL, 0);
    send_result = atoi(argv[3]);
    IHS_Init();
    IHS_Session *plain = IHS_TestSessionCreate();
    assert(plain);
    IHS_UnreliableChannel *generic = IHS_SessionUnreliableGet(plain, 3, 2048);
    assert(generic && generic->maxReceiveDelay == 65);
    IHS_SessionDestroy(plain);
    IHS_Session *session = IHS_TestSessionCreate();
    assert(session && !session->unreliable[3]);
    assert(IHS_SessionConfigureNetworkTest(session, mode, 20000));
    assert(!session->unreliable[3]); /* Configure is not the Connect trigger. */
    if (argc == 5) {
        fail_receiver = true;
        assert(!IHS_SessionConnect(session));
        IHS_NetworkTestResult failed;
        IHS_SessionGetNetworkTestResult(session, &failed);
        assert(failed.status == IHS_NetworkTestFailed && !failed.valid && !session->base.worker);
        puts("{\"local_failure_guard\":true,\"status\":4,\"worker\":false,\"valid\":false}");
        IHS_SessionDestroy(session);
        IHS_Quit();
        pthread_mutex_destroy(&lock);
        return 0;
    }
    assert(IHS_SessionConnect(session));
    bool sent = false;
    for (unsigned i = 0; i < 2000; ++i) {
        pthread_mutex_lock(&lock);
        sent = size != 0;
        pthread_mutex_unlock(&lock);
        if (sent)
            break;
        const struct timespec pause = {.tv_nsec = 1000000};
        nanosleep(&pause, NULL);
    }
    assert(sent);
    IHS_BaseLock(&session->base);
    assert(session->unreliable[3]);
    unsigned delay = session->unreliable[3]->maxReceiveDelay;
    assert(session->numChannels == 3 && !session->callbacks.video && !session->callbacks.audio);
    IHS_BaseUnlock(&session->base);
    IHS_SessionDisconnect(session);
    IHS_SessionThreadedJoin(session);
    IHS_NetworkTestResult result;
    IHS_SessionGetNetworkTestResult(session, &result);
    printf("{\"delay\":%u,\"status\":%u,\"phase\":%u,\"wire\":\"", delay, result.status,
           session->networkTest.phase);
    /* Source's connection-ID allocator is explicit12, Native's real allocator
     * is process-local. Normalize only that opaque identifier for comparison. */
    first[2] = 0;
    for (unsigned i = 0; i < size; ++i)
        printf("%02x", first[i]);
    puts("\"}");
    IHS_SessionDestroy(session);
    IHS_Quit();
    pthread_mutex_destroy(&lock);
    return 0;
}
