/* Local capture ownership and stop-vs-submit synchronization, not device proof.
 * Real protocol APIs/crypto/queues run; no host or capture/encoder is present. */
#include "ihslib.h"
#include "test_session.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    IHS_Session *session;
    IHS_Thread *worker;
    IHS_Mutex *gate;
    IHS_Cond *changed;
    bool requested, done, probeData, blocked;
    unsigned starts, stops;
} Capture;

static void capture(void *context) {
    Capture *c = context;
    IHS_MutexLock(c->gate);
    while (!c->requested)
        IHS_CondWait(c->changed, c->gate);
    IHS_MutexUnlock(c->gate);
    if (c->probeData) {
        const uint8_t data = 1;
        /* Needs Base. A stop callback that joins under Base would block. */
        assert(!IHS_SessionSendMicrophoneData(c->session, &data, 1));
    }
    IHS_MutexLock(c->gate);
    c->done = true;
    IHS_CondSignal(c->changed);
    IHS_MutexUnlock(c->gate);
}

static int start(IHS_Session *s, const IHS_StreamAudioConfig *config, void *context) {
    Capture *c = context;
    if (c->blocked)
        return 1; /* Let the caller join an old blocked worker after API unlock. */
    assert(!c->worker && config->codec == IHS_StreamAudioCodecOpus);
    c->session = s;
    c->requested = c->done = false;
    c->worker = IHS_ThreadCreate(capture, "MicOwnerTest", c);
    assert(c->worker);
    ++c->starts;
    return 0;
}

static void stop(IHS_Session *s, void *context) {
    Capture *c = context;
    assert(s == c->session && c->worker);
    IHS_MutexLock(c->gate);
    c->requested = true;
    IHS_CondSignal(c->changed);
    while (!c->done)
        if (!IHS_CondWaitTimeout(c->changed, c->gate, 2000))
            break;
    bool done = c->done;
    IHS_MutexUnlock(c->gate);
    ++c->stops;
    if (!done) {
        c->blocked = true;
        return; /* Normal failure path joins after the protocol API releases Base. */
    }
    IHS_ThreadJoin(c->worker);
    c->worker = NULL;
}

int main(void) {
    IHS_Init();
    IHS_Session *s = IHS_TestSessionCreate();
    s->state.connectionState = IHS_SessionConnectionStateConnected;
    s->negotiatedConfig = cnegotiated_config__unpack(NULL, 0, NULL);
    assert(s->negotiatedConfig);
    s->negotiatedConfig->selected_audio_codec = k_EStreamAudioCodecOpus;
    Capture c = {.gate = IHS_MutexCreate(), .changed = IHS_CondCreate(), .probeData = true};
    assert(c.gate && c.changed);
    static const IHS_StreamMicrophoneCallbacks callbacks = {.start = start, .stop = stop};
    IHS_SessionSetMicrophoneCallbacks(s, &callbacks, &c);
    const IHS_StreamAudioConfig config = {.codec = IHS_StreamAudioCodecOpus, .frequency = 48000, .channels = 2};
    assert(IHS_SessionStartMicrophoneStream(s, &config));
    assert(IHS_SessionStopMicrophoneStream(s));
    if (c.blocked)
        goto blocked;
    assert(!c.worker && c.starts == 1 && c.stops == 1);
    assert(!IHS_SessionStopMicrophoneStream(s));
    assert(IHS_SessionStartMicrophoneStream(s, &config));
    bool replaced = IHS_SessionStartMicrophoneStream(s, &config);
    if (c.blocked)
        goto blocked;
    assert(replaced && c.starts == 3 && c.stops == 2);
    assert(IHS_SessionStopMicrophoneStream(s));
    if (c.blocked)
        goto blocked;
    assert(!c.worker && c.stops == 3);
    /* Destruction must stop/join active capture before freeing Base/Socket.
     * This phase does not block on Base, even under the isolated lock rollback. */
    c.probeData = false;
    assert(IHS_SessionStartMicrophoneStream(s, &config));
    IHS_SessionDestroy(s);
    assert(!c.worker && c.starts == 4 && c.stops == 4);
    IHS_CondDestroy(c.changed);
    IHS_MutexDestroy(c.gate);
    IHS_Quit();
    puts("Microphone stop/replacement release Base; active capture joins before session destruction");
    return 0;
blocked:
    IHS_ThreadJoin(c.worker);
    c.worker = NULL;
    IHS_SessionDestroy(s);
    IHS_CondDestroy(c.changed);
    IHS_MutexDestroy(c.gate);
    IHS_Quit();
    fputs("Microphone stop callback blocked a producer on Base\n", stderr);
    return 1;
}
