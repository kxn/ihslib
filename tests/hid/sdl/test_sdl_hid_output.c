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
#include "ihs_enumeration.h"
#include "session/channels/ch_control.h"

typedef struct fixture {
    SDL_GameController *controller;
    SDL_JoystickID id;
    pthread_t owner;
    unsigned native_calls, effects, triggers, discontinuities;
    unsigned prefix_received, expected_prefix;
    bool check_prefix;
    uint8_t events[512];
    unsigned event_count;
    bool reject;
    bool native_available;
    uint64_t previous_ns;
    uint8_t copied[69];
    size_t copied_size;
} fixture;
#ifdef IHS_TEST_CLOCK_WRAP
static uint32_t advertised_caps;
static uint16_t expected_vendor, expected_product;
static unsigned advertisements;
bool __real_IHS_SessionChannelControlSend(IHS_SessionChannel *, EStreamControlMessage,
                                          const ProtobufCMessage *, int32_t);
bool __wrap_IHS_SessionChannelControlSend(IHS_SessionChannel *channel, EStreamControlMessage type,
                                          const ProtobufCMessage *message, int32_t packet_id) {
    if (type != k_EStreamControlRemoteHID)
        return __real_IHS_SessionChannelControlSend(channel, type, message, packet_id);
    const CRemoteHIDMsg *wrapped = (const void *)message;
    CHIDMessageFromRemote *hid = chidmessage_from_remote__unpack(NULL, wrapped->data.len, wrapped->data.data);
    assert(hid);
    if (hid->command_case != CHIDMESSAGE_FROM_REMOTE__COMMAND_UPDATE_DEVICE_LIST) {
        chidmessage_from_remote__free_unpacked(hid, NULL);
        return __real_IHS_SessionChannelControlSend(channel, type, message, packet_id);
    }
    assert(hid->update_device_list->n_devices == 1);
    const CHIDDeviceInfo *device = hid->update_device_list->devices[0];
    assert(device->is_generic_gamepad && device->vendor_id == expected_vendor &&
           device->product_id == expected_product);
    advertised_caps = device->caps_bits;
    ++advertisements;
    chidmessage_from_remote__free_unpacked(hid, NULL);
    return true;
}
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
static bool native_capability(SDL_JoystickID id, void *p) {
    fixture *f = p;
    assert(id == f->id);
    return f->native_available;
}
static void check_capability(IHS_HIDProvider *provider, IHS_Session *session, bool expected) {
    IHS_Enumeration *enumeration = IHS_HIDProviderEnumerateDevices(provider);
    IHS_HIDDeviceInfo info = {.native_haptic_pulse = !expected};
    IHS_HIDProviderDeviceInfo(provider, enumeration, &info);
    assert(info.native_haptic_pulse == expected);
    IHS_EnumerationFree(enumeration);
#ifdef IHS_TEST_CLOCK_WRAP
    unsigned before = advertisements;
    expected_vendor = info.vendor_id;
    expected_product = info.product_id;
    int previous_state = session->state.connectionState;
    bool previous_input = session->state.streamingInput;
    session->state.connectionState = IHS_SessionConnectionStateConnected;
    session->state.streamingInput = true;
    assert(IHS_SessionHIDNotifyDeviceChange(session));
    assert(advertisements == before + 1);
    assert(advertised_caps == (0x8343ffu | (expected ? 0x2000u : 0)));
    session->state.connectionState = previous_state;
    session->state.streamingInput = previous_input;
#else
    (void)session;
#endif
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
    assert(f->event_count < sizeof(f->events));
    f->events[f->event_count++] = bytes[0];
    if (f->check_prefix) {
        assert(bytes[0] == 13 && size == 69 && bytes[6] == f->prefix_received);
        ++f->prefix_received;
    }
    return f->reject ? -1 : 0;
}
static void output_gap(SDL_JoystickID id, void *p) {
    fixture *f = p;
    assert(id == f->id && pthread_equal(f->owner, pthread_self()));
    assert(f->check_prefix && f->prefix_received == f->expected_prefix);
    assert(f->event_count < sizeof(f->events));
    f->events[f->event_count++] = 0xff;
    ++f->discontinuities;
}
static int rumble_triggers(void *p, Uint16 left, Uint16 right) {
    fixture *f = p;
    assert(pthread_equal(f->owner, pthread_self()));
    if (!left && !right)
        return 0; /* SDL stops the virtual actuator when closing the device. */
    assert(left == 0x1234 && right == 0xabcd);
    ++f->triggers;
    return 0;
}
static int effect(void *p, const void *bytes, int size) {
    fixture *f = p;
    assert(pthread_equal(f->owner, pthread_self()));
    assert(size >= 0 && size <= 64);
    memcpy(f->copied, bytes, (size_t)size);
    f->copied_size = (size_t)size;
    ++f->effects;
    if (f->check_prefix) {
        assert(size == 10 && ((const uint8_t *)bytes)[1] == f->prefix_received);
        ++f->prefix_received;
    }
    return f->reject ? -1 : 0;
}
static void check(bool native, bool notify_gap) {
    fixture f = {.owner = pthread_self()};
    SDL_VirtualJoystickDesc desc = {.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION,
        .type = SDL_JOYSTICK_TYPE_GAMECONTROLLER, .naxes = 6, .nbuttons = 16,
        .vendor_id = 0x057e, .product_id = 0x2009,
        .name = "IHS output fixture", .userdata = &f, .SendEffect = effect,
        .RumbleTriggers = rumble_triggers};
    int index = SDL_JoystickAttachVirtualEx(&desc);
    assert(index >= 0);
    f.controller = SDL_GameControllerOpen(index);
    assert(f.controller);
    f.id = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(f.controller));
    IHS_HIDProviderSDLDeviceList list = {.count = device_count, .index = device_index,
        .instanceId = device_id, .controller = device_controller,
        .output = native ? native_output : NULL,
        .outputDiscontinuity = notify_gap ? output_gap : NULL};
    IHS_HIDProvider *provider = IHS_HIDProviderSDLCreateUnmanaged(&list, &f);
    IHS_Session *session = IHS_TestSessionCreate();
    IHS_SessionHIDAddProvider(session, provider);
    check_capability(provider, session, false); /* output alone is insufficient */
    list.supportsNativeHaptics = native_capability;
    check_capability(provider, session, false); /* unavailable backend */
    f.native_available = true;
    check_capability(provider, session, native); /* requires callback AND output */
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
    const uint8_t triggers[21] = {6, 0x34, 0x12, 0xcd, 0xab, 10};
    assert(IHS_HIDDeviceWrite(managed->device, triggers, sizeof(triggers)) == 0);
    assert(!f.triggers && f.native_calls == (native ? 1u : 0u));
    IHS_HIDSDLApplyPendingWrites(session);
    if (native) {
        assert(f.native_calls == 2 && !f.triggers && !f.effects);
        assert(f.copied_size == 9 && !memcmp(f.copied, triggers, 9));
        assert(f.events[1] == 6);
    } else {
        assert(f.triggers == 1 && !f.native_calls);
    }
    memcpy(command, original, 69);
    assert(IHS_HIDDeviceWrite(managed->device, command, 68) == -1);
    command[1] = 65;
    assert(IHS_HIDDeviceWrite(managed->device, command, 69) == -1);
    command[1] = 10; command[4] = 1;
    assert(IHS_HIDDeviceWrite(managed->device, command, 69) == -1);
    memcpy(command, original, 69);
    unsigned accepted = 0;
    f.check_prefix = true;
    for (;;) {
        command[6] = (uint8_t)accepted;
        if (IHS_HIDDeviceWrite(managed->device, command, 69) < 0)
            break;
        ++accepted;
    }
    assert(accepted == 8192u / (69u + sizeof(uint64_t)));
    f.expected_prefix = accepted;
    assert(f.prefix_received == 0 && f.discontinuities == 0);
    IHS_HIDSDLOutputStats stats;
    IHS_HIDSDLGetOutputStats(session, &stats);
    assert(stats.queueDropped == 1 && stats.pendingBytes == accepted * (69u + sizeof(uint64_t)));
    /* A small configuration write still fits after the rejected large write.
     * Stateful backends must reject it until the owner observes the gap;
     * callers without a gap callback retain their existing SDL behavior. */
    const uint8_t player[2] = {0x0b, 1};
    assert(IHS_HIDDeviceWrite(managed->device, player, sizeof(player)) == (notify_gap ? -1 : 0));
    IHS_HIDSDLApplyPendingWrites(session);
    assert(f.prefix_received == accepted);
    assert(f.discontinuities == (notify_gap ? 1u : 0u));
    if (notify_gap)
        assert(f.events[f.event_count - 1] == 0xff);
    else
        assert(SDL_GameControllerGetPlayerIndex(f.controller) == 1);
    f.check_prefix = false;
    IHS_HIDSDLGetOutputStats(session, &stats);
    assert(stats.queued == accepted + 2 + (notify_gap ? 0u : 1u) &&
           stats.applied == stats.queued && !stats.pendingBytes);
    assert(stats.queueDropped == (notify_gap ? 2u : 1u));
    IHS_HIDSDLApplyPendingWrites(session);
    assert(f.discontinuities == (notify_gap ? 1u : 0u)); /* gap is consumed once */
    f.reject = true;
    unsigned events_before = f.event_count;
    assert(IHS_HIDDeviceWrite(managed->device, command, 69) == 0);
    IHS_HIDSDLApplyPendingWrites(session);
    if (native)
        assert(f.event_count == events_before + 1 && f.events[events_before] == 13);
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
    check(false, false);
    check(true, false);
    check(true, true);
    SDL_Quit();
    IHS_Quit();
    puts("PASS effect bounds, ownership, FIFO, trigger routing, SDL fallback, 8192-byte bound and ordered gaps");
}
