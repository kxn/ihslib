#include <SDL2/SDL.h>
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "ihslib/hid/sdl.h"
#include "hid/device.h"
#include "hid/manager.h"
#include "hid/provider.h"
#include "test_session.h"

typedef struct fixture {
    SDL_GameController *controller;
    SDL_JoystickID id;
    pthread_t owner;
    unsigned native_calls, effects;
    bool reject;
    uint64_t previous_ns;
    uint8_t copied[69];
    size_t copied_size;
} fixture;
#ifdef IHS_TEST_CLOCK_WRAP
static _Thread_local bool watch_clock;
static atomic_bool clock_seen;
static uint64_t entry_ns;
int __real_clock_gettime(clockid_t clock, struct timespec *t);
int __wrap_clock_gettime(clockid_t clock, struct timespec *t) {
    int result = __real_clock_gettime(clock, t);
    if (!result && clock == CLOCK_MONOTONIC && watch_clock) {
        entry_ns = (uint64_t)t->tv_sec * 1000000000ull + (uint64_t)t->tv_nsec;
        atomic_store(&clock_seen, true);
    }
    return result;
}
typedef struct writer {
    IHS_HIDDevice *device;
    const uint8_t *command;
    int result;
} writer;
static void *write_while_owner_locked(void *p) {
    writer *w = p;
    watch_clock = true;
    w->result = IHS_HIDDeviceWrite(w->device, w->command, 69);
    watch_clock = false;
    return NULL;
}
#endif
static int device_count(void *p) { (void)p; return 1; }
static int device_index(SDL_JoystickID id, void *p) { return ((fixture *)p)->id == id ? 0 : -1; }
static SDL_JoystickID device_id(int index, void *p) { (void)index; return ((fixture *)p)->id; }
static SDL_GameController *device_controller(int index, void *p) {
    return index == 0 ? ((fixture *)p)->controller : NULL;
}
static int native_output(SDL_JoystickID id, const uint8_t *bytes, size_t size,
                          uint64_t received, void *p) {
    fixture *f = p;
    assert(id == f->id && pthread_equal(f->owner, pthread_self()));
    assert(received && received >= f->previous_ns);
    f->previous_ns = received;
    assert(size <= sizeof(f->copied));
    memcpy(f->copied, bytes, size);
    f->copied_size = size;
    ++f->native_calls;
    return f->reject ? -1 : 0;
}
static int effect(void *p, const void *bytes, int size) {
    fixture *f = p;
    assert(pthread_equal(f->owner, pthread_self()));
    assert(size >= 0 && size <= 64);
    memcpy(f->copied, bytes, (size_t)size);
    f->copied_size = (size_t)size;
    ++f->effects;
    return f->reject ? -1 : 0;
}
static void check(bool native) {
    fixture f = {.owner = pthread_self()};
    SDL_VirtualJoystickDesc desc = {.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION,
        .type = SDL_JOYSTICK_TYPE_GAMECONTROLLER, .naxes = 6, .nbuttons = 16,
        .name = "IHS output fixture", .userdata = &f, .SendEffect = effect};
    int index = SDL_JoystickAttachVirtualEx(&desc);
    assert(index >= 0);
    f.controller = SDL_GameControllerOpen(index);
    assert(f.controller);
    f.id = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(f.controller));
    IHS_HIDProviderSDLDeviceList list = {.count = device_count, .index = device_index,
        .instanceId = device_id, .controller = device_controller,
        .output = native ? native_output : NULL};
    IHS_HIDProvider *provider = IHS_HIDProviderSDLCreateUnmanaged(&list, &f);
    IHS_Session *session = IHS_TestSessionCreate();
    IHS_SessionHIDAddProvider(session, provider);
    char path[48];
    snprintf(path, sizeof(path), "sdl://%d", f.id);
    IHS_HIDManagedDevice *managed = IHS_HIDManagerOpenDevice(session->hidManager, path);
    assert(managed);
    uint8_t command[69] = {13, 10, 0, 0, 0, 0x10, 0,
        0, 1, 0x40, 0x40, 0, 0, 5, 0xc0};
    uint8_t original[69];
    memcpy(original, command, 69);
    assert(IHS_HIDDeviceWrite(managed->device, command, 69) == 0);
    memset(command, 0xff, 69); /* receive buffer lifetime must not leak into drain */
    assert(!f.native_calls && !f.effects);
    IHS_HIDSDLApplyPendingWrites(session);
    assert(native ? f.native_calls == 1 && !f.effects : f.effects == 1 && !f.native_calls);
    assert(f.copied_size == (native ? 69 : 10));
    assert(memcmp(f.copied, original + (native ? 0 : 5), f.copied_size) == 0);
    memcpy(command, original, 69);
    assert(IHS_HIDDeviceWrite(managed->device, command, 68) == -1);
    command[1] = 65;
    assert(IHS_HIDDeviceWrite(managed->device, command, 69) == -1);
    command[1] = 10; command[4] = 1;
    assert(IHS_HIDDeviceWrite(managed->device, command, 69) == -1);
    memcpy(command, original, 69);
    unsigned accepted = 0;
    while (IHS_HIDDeviceWrite(managed->device, command, 69) == 0)
        ++accepted;
    assert(accepted && accepted < 8);
    IHS_HIDSDLOutputStats stats;
    IHS_HIDSDLGetOutputStats(session, &stats);
    assert(stats.queueDropped == 1 && stats.pendingBytes);
    IHS_HIDSDLApplyPendingWrites(session);
    IHS_HIDSDLGetOutputStats(session, &stats);
    assert(stats.queued == accepted + 1 && stats.applied == stats.queued && !stats.pendingBytes);
    f.reject = true;
    assert(IHS_HIDDeviceWrite(managed->device, command, 69) == 0);
    IHS_HIDSDLApplyPendingWrites(session);
    IHS_HIDSDLGetOutputStats(session, &stats);
    assert(stats.rejected == 1);
    assert(!native || !f.effects);
#ifdef IHS_TEST_CLOCK_WRAP
    if (native) {
        f.reject = false;
        atomic_store(&clock_seen, false);
        writer w = {.device = managed->device, .command = command};
        pthread_t receiver;
        IHS_HIDDeviceLock(managed->device);
        assert(pthread_create(&receiver, NULL, write_while_owner_locked, &w) == 0);
        for (unsigned i = 0; i < 500 && !atomic_load(&clock_seen); ++i) {
            struct timespec pause = {.tv_nsec = 1000000};
            nanosleep(&pause, NULL);
        }
        bool before_unlock = atomic_load(&clock_seen);
        IHS_HIDDeviceUnlock(managed->device);
        pthread_join(receiver, NULL);
        assert(before_unlock && w.result == 0);
        IHS_HIDSDLApplyPendingWrites(session);
        assert(f.previous_ns == entry_ns);
    }
#endif
    IHS_SessionDestroy(session);
    IHS_HIDProviderSDLDestroy(provider);
    SDL_GameControllerClose(f.controller);
    SDL_JoystickDetachVirtual(index);
}
int main(void) {
    IHS_Init();
    assert(SDL_Init(SDL_INIT_GAMECONTROLLER) == 0);
    check(false);
    check(true);
    SDL_Quit();
    IHS_Quit();
    puts("PASS effect envelope bounds, ownership, FIFO, native dispatch, SDL fallback and overflow");
}
