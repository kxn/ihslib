/* Public guards/full-sample ownership, actual producer vs concurrent bounded
 * readers, and targeted resource failure. No device/host scheduling claim. */
#include "ihs_buffer.h"
#include "ihs_buffer_ext.h"
#include "ihslib.h"
#include "session/channels/ch_control.h"
#include "session/session_pri.h"
#include "test_session.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
static _Thread_local bool failNext;
void *__real_realloc(void *, size_t);
void *__wrap_realloc(void *p, size_t size) {
    if (failNext) {
        failNext = false;
        return NULL;
    }
    return __real_realloc(p, size);
}
static IHS_Session *s;
static atomic_bool done;
static unsigned failAdmission, admissions;
IHS_SendAdmission __real_IHS_SessionChannelQueueFrameResult(IHS_SessionChannel *,
                                                            IHS_SessionFrame *, bool);
IHS_SendAdmission __wrap_IHS_SessionChannelQueueFrameResult(IHS_SessionChannel *channel,
                                                            IHS_SessionFrame *frame,
                                                            bool reliable) {
    if (failAdmission)
        return ++admissions == failAdmission ? IHS_SendAdmissionLocalFailure
                                             : IHS_SendAdmissionAccepted;
    return __real_IHS_SessionChannelQueueFrameResult(channel, frame, reliable);
}
static void *reader(void *unused) {
    (void)unused;
    for (unsigned i = 0; i < 512 && !atomic_load(&done); ++i) {
        IHS_NetworkTestResult snapshot;
        IHS_SessionGetNetworkTestResult(s, &snapshot);
        assert(snapshot.valid && snapshot.status == IHS_NetworkTestRunning);
        IHS_NetworkTestSample samples[32];
        size_t n = IHS_SessionReadNetworkTestSamples(s, 0, samples, 32);
        assert(n <= 32);
        for (size_t j = 0; j < n; ++j)
            assert(samples[j].targetKbps == 100000 && samples[j].networkTicks == 65);
    }
    return NULL;
}
static IHS_Session *create(void) {
    IHS_Session *session = IHS_TestSessionCreate();
    assert(session);
    IHS_TimerTaskStopImmediate(session->retransmission.timer);
    return session;
}
static void record(IHS_SessionFrame *f, unsigned id) {
    uint16_t word = id;
    memcpy(IHS_BufferPointerAt(&f->body, 1), &word, 2);
    IHS_SessionNetworkTestRecord(s, f);
}
int main(void) {
    IHS_Init();
    IHS_NetworkTestResult result;
    memset(&result, 255, sizeof(result));
    IHS_SessionGetNetworkTestResult(NULL, &result);
    assert(!result.valid && !result.samples);
    IHS_SessionGetNetworkTestResult(NULL, NULL);
    assert(!IHS_SessionConfigureNetworkTest(NULL, IHS_NetworkTestFixed, 15000));
    s = create();
    assert(!IHS_SessionConfigureNetworkTest(s, 0, 15000));
    assert(!IHS_SessionConfigureNetworkTest(s, IHS_NetworkTestFixed, 0));
    assert(!IHS_SessionConfigureNetworkTest(s, IHS_NetworkTestFixed, 0x80000000u));
    s->info.sessionKeyLen = 16;
    assert(!IHS_SessionConfigureNetworkTest(s, IHS_NetworkTestFixed, 15000));
    s->info.sessionKeyLen = 32;
    assert(IHS_SessionConfigureNetworkTest(s, IHS_NetworkTestBandwidth, 15000));
    IHS_SessionNetworkTestBegin(s);
    s->state.connectionState = IHS_SessionConnectionStateNegotiating;
    s->networkTest.phase = 4;
    assert(!IHS_SessionConfigureNetworkTest(s, IHS_NetworkTestFixed, 10000));
    IHS_SessionNetworkTestTarget(s, 100000);
    IHS_SessionFrame frame = {.header = {.sendTimestamp = 1000, .receiveTimestamp = 1065}};
    IHS_BufferInit(&frame.body, 32, 32);
    IHS_BufferAppendUInt8(&frame.body, k_EStreamDataPacket);
    IHS_BufferFillMem(&frame.body, 1, 0, 12);
    pthread_t worker;
    assert(!pthread_create(&worker, NULL, reader, NULL));
    for (unsigned i = 0; i < 2000; ++i)
        record(&frame, i);
    atomic_store(&done, true);
    assert(!pthread_join(worker, NULL));
    IHS_SessionGetNetworkTestResult(s, &result);
    assert(result.samples == 2000 && result.frameSpan == 2000);
    IHS_NetworkTestSample output[300];
    assert(IHS_SessionReadNetworkTestSamples(s, 0, output, 300) == 256);
    assert(!IHS_SessionReadNetworkTestSamples(s, SIZE_MAX, output, 300));
    assert(!IHS_SessionReadNetworkTestSamples(s, 0, NULL, 300));
    IHS_SessionDisconnect(s);
    IHS_SessionNetworkTestFinalize(s);
    IHS_SessionGetNetworkTestResult(s, &result);
    assert(result.status == IHS_NetworkTestCancelled && !s->mediaNetworkStats.finalized);
    IHS_SessionDestroy(s);
    s = create();
    assert(IHS_SessionConfigureNetworkTest(s, IHS_NetworkTestFixed, 15000));
    IHS_SessionNetworkTestBegin(s);
    s->state.connectionState = IHS_SessionConnectionStateNegotiating;
    s->networkTest.phase = 4;
    failNext = true;
    record(&frame, 0);
    assert(!failNext);
    IHS_SessionGetNetworkTestResult(s, &result);
    assert(!result.valid && !result.samples && result.frameSpan == 1);
    IHS_SessionNetworkTestUpdate(s, 1000);
    IHS_SessionNetworkTestFinalize(s);
    IHS_SessionGetNetworkTestResult(s, &result);
    assert(result.status == IHS_NetworkTestFailed && !result.valid);
    IHS_SessionDestroy(s);
    /* Local send preparation failure is not a caller cancellation. Exercise
     * auth, each of the three keepalives, request8, and the stats-channel send.
     * Final admission is the only adapter; encryption/producers/cleanup run. */
    for (unsigned scenario = 0; scenario < 7; ++scenario) {
        s = create();
        assert(IHS_SessionConfigureNetworkTest(s, IHS_NetworkTestFixed, 15000));
        IHS_SessionNetworkTestBegin(s);
        s->networkTest.phase = 4;
        s->state.connectionState = IHS_SessionConnectionStateHandshaking;
        admissions = 0;
        failAdmission = scenario >= 1 && scenario <= 4 ? scenario : 1;
        if (!scenario)
            IHS_SessionChannelControlRequestAuthentication(s->channels[1]);
        else if (scenario <= 4)
            IHS_SessionNetworkTestAuthenticated(s);
        else if (scenario == 5)
            IHS_SessionNetworkTestUpdate(s, 1000);
        else {
            s->networkTest.lastData = s->networkTest.frameEventsTick = 70000;
            IHS_SessionNetworkTestUpdate(s, 70000);
        }
        assert(admissions == failAdmission && s->base.interrupted);
        assert(!s->networkTest.phase && !s->networkTest.keepAliveTick &&
               !s->networkTest.frameEventsTick && !s->networkTest.lastData);
        failAdmission = 0;
        IHS_SessionNetworkTestFinalize(s);
        IHS_SessionGetNetworkTestResult(s, &result);
        assert(result.status == IHS_NetworkTestFailed && !s->mediaNetworkStats.finalized);
        IHS_SessionDestroy(s);
    }
    IHS_BufferClear(&frame.body, true);
    IHS_Quit();
    return 0;
}
