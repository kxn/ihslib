/* Real SDL events -> state/holder -> actual control encryption and queue.
 * Only SDL handle/joystick-ID lookups are adapters, no physical device/UDP. */
#include "common/test_session.h"
#include "hid/device.h"
#include "hid/manager.h"
#include "hid/sdl/src/sdl_hid_common.h"
#include "ihslib/hid/sdl.h"
#include "session/channels/ch_control.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    IHS_SessionPacket packet;
    bool reliable;
} QueuedPacket;
SDL_Joystick *__wrap_SDL_GameControllerGetJoystick(SDL_GameController *gamepad) {
    (void)gamepad;
    return (SDL_Joystick *)(uintptr_t)2;
}
SDL_JoystickID __wrap_SDL_JoystickInstanceID(SDL_Joystick *joystick) {
    (void)joystick;
    return 42;
}
static void hex(const uint8_t *data, size_t size) {
    for (size_t i = 0; i < size; ++i)
        printf("%02x", data[i]);
}
static void observe(IHS_Session *s, IHS_HIDManagedDevice *m) {
    IHS_SessionChannelControl *control = (void *)s->channels[1];
    printf("{\"sequence\":%llu,\"bodies\":[", (unsigned long long)control->sendEncryptSequence);
    unsigned count = 0;
    QueuedPacket *q;
    while ((q = (void *)IHS_QueuePoll(s->sendQueue))) {
        assert(q->reliable && q->packet.header.channelId == 1 && q->packet.header.fragmentId == 0);
        printf("%s\"", count++ ? "," : "");
        hex(IHS_BufferPointer(&q->packet.body), q->packet.body.size);
        printf("\"");
        IHS_SessionPacketClear(&q->packet, true);
        IHS_QueueItemFree((void *)q);
    }
    uint8_t previous[72];
    IHS_HIDDeviceSDL *device = (void *)m->device;
    IHS_HIDReportSDLPackWire(previous, sizeof(previous), &device->states.previous);
    printf("],\"baseline\":\"");
    hex(previous, sizeof(previous));
    printf("\",\"pending_reports\":%zu,\"diagnostic_sequence\":%llu}\n",
           m->reportHolder.reportItems.size, (unsigned long long)device->lastSubmittedSeq);
}
int main(void) {
    IHS_Init();
    IHS_Session *s = IHS_TestSessionCreate();
    assert(s);
    IHS_TimerTaskStopImmediate(s->retransmission.timer);
    s->state.connectionState = IHS_SessionConnectionStateConnected;
    IHS_HIDManagedDevice *m = calloc(1, sizeof(*m));
    assert(m);
    m->manager = s->hidManager;
    m->id = 1;
    m->lock = IHS_MutexCreate();
    m->device = IHS_HIDDeviceSDLCreate(NULL, (SDL_Gamepad *)(uintptr_t)1, false);
    assert(m->device && m->lock);
    m->device->managed = m;
    IHS_HIDReportHolderInit(&m->reportHolder, 1);
    m->device->cls->opened(m->device);
    IHS_ArrayListAppend(&s->hidManager->devices, &m);
    unsigned enabled, buttons, step = 0;
    int axes[6];
    while (scanf("%u %u %d %d %d %d %d %d", &enabled, &buttons, &axes[0], &axes[1], &axes[2],
                 &axes[3], &axes[4], &axes[5]) == 8) {
        assert(enabled <= 1 && buttons <= 65535);
        const uint8_t config[] = {0x58, (uint8_t)enabled};
        assert(IHS_SessionSetStreamingClientConfig(s, config, sizeof(config), true));
        for (unsigned bit = 0; bit < 16; ++bit) {
            SDL_Event e = {0};
            e.type = (buttons & (1u << bit)) ? SDL_EVENT_GAMEPAD_BUTTON_DOWN
                                             : SDL_EVENT_GAMEPAD_BUTTON_UP;
            e.gbutton.which = 42;
            e.gbutton.button = bit;
            e.gbutton.down = !!(buttons & (1u << bit));
            IHS_HIDHandleSDLEvent(s, &e);
        }
        for (unsigned axis = 0; axis < 6; ++axis) {
            assert(axes[axis] >= -32768 && axes[axis] <= 32767);
            SDL_Event e = {0};
            e.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
            e.gaxis.which = 42;
            e.gaxis.axis = axis;
            e.gaxis.value = axes[axis];
            IHS_HIDHandleSDLEvent(s, &e);
        }
        if (!step++) {
            assert(IHS_HIDDeviceStartInputReports(m->device, 72) == 0);
            IHS_SessionHIDSendReport(s);
        } else {
            IHS_HIDFlushSDLGameControllers(s);
        }
        observe(s, m);
    }
    assert(feof(stdin));
    IHS_SessionDestroy(s);
    IHS_Quit();
    return 0;
}
