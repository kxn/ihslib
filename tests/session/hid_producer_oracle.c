/* Production HID API -> protobuf -> encryption -> queue, with real holder
 * collection in batch mode. No socket or sender worker is started. */
#include "common/test_session.h"
#include "hid/device.h"
#include "hid/manager.h"
#include "session/channels/ch_control.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    IHS_SessionPacket packet;
    bool reliable;
} QueuedPacket;
static void close_device(IHS_HIDDevice *device) {
    (void)device;
}
static void free_device(IHS_HIDDevice *device) {
    free(device);
}
static void hex(const uint8_t *bytes, size_t size) {
    for (size_t i = 0; i < size; ++i)
        printf("%02x", bytes[i]);
}
static IHS_HIDManagedDevice *add_device(IHS_Session *s, bool active) {
    static const IHS_HIDDeviceClass cls = {.close = close_device, .free = free_device};
    IHS_HIDManagedDevice *m = calloc(1, sizeof(*m));
    assert(m);
    m->manager = s->hidManager;
    m->id = 1;
    m->lock = IHS_MutexCreate();
    m->device = calloc(1, sizeof(*m->device));
    assert(m->lock && m->device);
    m->device->cls = &cls;
    m->device->managed = m;
    m->reportActivityKnown = true;
    m->reportActiveInput = active;
    IHS_HIDReportHolderInit(&m->reportHolder, 1);
    IHS_HIDReportHolderSetReportLength(&m->reportHolder, 3);
    const uint8_t data[] = {1, 2, 3};
    IHS_HIDReportHolderAddFull(&m->reportHolder, data, sizeof(data));
    IHS_ArrayListAppend(&s->hidManager->devices, &m);
    return m;
}
int main(int argc, char **argv) {
    assert(argc == 6);
    unsigned state = (unsigned)atoi(argv[2]);
    bool enabled = atoi(argv[3]) != 0, active = atoi(argv[4]) != 0;
    assert(state < 8 && strlen(argv[5]) <= 512 && strlen(argv[5]) % 2 == 0);
    uint8_t payload[256];
    size_t size = strlen(argv[5]) / 2;
    for (size_t n = 0; n < size; ++n) {
        unsigned byte;
        assert(sscanf(argv[5] + 2 * n, "%2x", &byte) == 1);
        payload[n] = byte;
    }
    IHS_Init();
    IHS_Session *s = IHS_TestSessionCreate();
    assert(s);
    IHS_TimerTaskStopImmediate(s->retransmission.timer);
    s->state.connectionState = state;
    s->state.streamingInput = enabled;
    IHS_SessionChannelControl *control = (void *)s->channels[1];
    IHS_HIDManagedDevice *device = NULL;
    bool accepted;
    if (!strcmp(argv[1], "generic")) {
        CHIDMessageFromRemote *message = chidmessage_from_remote__unpack(NULL, size, payload);
        assert(message);
        accepted = IHS_SessionChannelControlSendHIDMsg(&control->base, message);
        chidmessage_from_remote__free_unpacked(message, NULL);
    } else if (!strcmp(argv[1], "report")) {
        accepted = IHS_SessionChannelControlSubmitHIDReport(&control->base, payload, size, active);
    } else {
        assert(!strcmp(argv[1], "batch"));
        device = add_device(s, active);
        accepted = IHS_SessionHIDSendReport(s);
    }
    printf("{\"accepted\":%s,\"sequence\":%llu,\"submitted\":%llu,\"sent\":%llu,\"pending_"
           "reports\":%zu,\"bodies\":[",
           accepted ? "true" : "false", (unsigned long long)control->sendEncryptSequence,
           (unsigned long long)control->hidSubmitted, (unsigned long long)control->hidSent,
           device ? device->reportHolder.reportItems.size : 0);
    QueuedPacket *queued;
    unsigned count = 0;
    while ((queued = (void *)IHS_QueuePoll(s->sendQueue))) {
        assert(queued->reliable && queued->packet.header.channelId == 1 &&
               queued->packet.header.fragmentId == 0);
        printf("%s\"", count++ ? "," : "");
        hex(IHS_BufferPointer(&queued->packet.body), queued->packet.body.size);
        printf("\"");
        IHS_SessionPacketClear(&queued->packet, true);
        IHS_QueueItemFree((void *)queued);
    }
    puts("]}");
    IHS_SessionDestroy(s);
    IHS_Quit();
    return 0;
}
