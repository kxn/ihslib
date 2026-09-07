/* Concurrent collectors must preserve the generated delta chain on the wire. */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "common/test_session.h"
#include "hid/manager.h"
#include "hid/device.h"
#include "crc32.h"
#include "session/channels/ch_control.h"

struct IHS_QueueItem { IHS_SessionPacket packet; bool reliable; };
typedef struct {
    IHS_Session *session;
    IHS_HIDManagedDevice *device;
    unsigned counter;
    uint8_t state[72];
} Fixture;
static void device_close(IHS_HIDDevice *device) { (void)device; }
static void device_free(IHS_HIDDevice *device) { free(device); }
static void generate(void *context) {
    Fixture *f = context;
    for (unsigned i = 0; i < 64; i++) {
        IHS_MutexLock(f->device->lock);
        uint8_t previous[72]; memcpy(previous, f->state, 72);
        unsigned n = ++f->counter;
        for (unsigned k = 0; k < 4; k++) f->state[k] = n >> (k*8);
        IHS_HIDReportHolderAddDelta(&f->device->reportHolder, previous, f->state, 72);
        IHS_MutexUnlock(f->device->lock);
        IHS_SessionHIDSendReport(f->session);
    }
}
int main(void) {
    IHS_Init();
    Fixture f = {.session = IHS_TestSessionCreate()};
    IHS_TimerTaskStopImmediate(f.session->retransmission.timer);
    IHS_SessionChannelControl *c = (void *)f.session->channels[1];
    IHS_TimerTaskStopImmediate(c->feedbackTimer);
    f.device = calloc(1, sizeof(*f.device));
    f.device->manager = f.session->hidManager;
    f.device->id = 1;
    f.device->lock = IHS_MutexCreate();
    static const IHS_HIDDeviceClass cls = {.close=device_close, .free=device_free};
    f.device->device = calloc(1, sizeof(IHS_HIDDevice));
    f.device->device->cls = &cls;
    f.device->device->managed = f.device;
    IHS_HIDReportHolderInit(&f.device->reportHolder, 1);
    IHS_HIDReportHolderSetReportLength(&f.device->reportHolder, 72);
    IHS_ArrayListAppend(&f.session->hidManager->devices, &f.device);
    IHS_Thread *workers[4];
    for (unsigned i = 0; i < 4; i++) workers[i] = IHS_ThreadCreate(generate, "HID.order", &f);
    for (unsigned i = 0; i < 4; i++) { assert(workers[i]); IHS_ThreadJoin(workers[i]); }
    IHS_SessionHIDSendReport(f.session);

    uint8_t reconstructed[72] = {0};
    unsigned applied = 0;
    uint64_t sequence = 0;
    IHS_QueueItem *q;
    while ((q = IHS_QueuePoll(f.session->sendQueue))) {
        assert(q->packet.header.fragmentId == 0);
        assert(*IHS_BufferPointer(&q->packet.body) == k_EStreamControlRemoteHID);
        IHS_BufferOffsetBy(&q->packet.body, 1);
        IHS_Buffer plain = IHS_BUFFER_INIT(2048, 2048);
        uint64_t actual;
        assert(IHS_SessionFrameDecrypt(f.session, &q->packet.body, &plain, sequence++, &actual) == 0);
        CRemoteHIDMsg *remote = cremote_hidmsg__unpack(NULL, plain.size, IHS_BufferPointer(&plain));
        assert(remote && remote->has_data);
        CHIDMessageFromRemote *hid = chidmessage_from_remote__unpack(NULL, remote->data.len, remote->data.data);
        assert(hid && hid->reports && hid->reports->n_device_reports == 1);
        IHS_HIDDeviceReportMessage *batch = hid->reports->device_reports[0];
        for (size_t i = 0; i < batch->n_reports; i++) {
            CHIDDeviceInputReport *report = batch->reports[i];
            if (report->has_full_report) {
                assert(report->full_report.len == 72);
                memcpy(reconstructed, report->full_report.data, 72);
            } else {
                assert(report->has_delta_report && report->delta_report.len >= 9);
                size_t at = 9;
                for (unsigned k = 0; k < 72; k++) {
                    if (report->delta_report.data[k/8] & (1u << (k%8))) {
                        assert(at < report->delta_report.len);
                        reconstructed[k] = report->delta_report.data[at++];
                    }
                }
                assert(at == report->delta_report.len);
                assert(report->delta_report_crc == IHS_CRC32(reconstructed, 72));
            }
            unsigned value = reconstructed[0] | (unsigned)reconstructed[1]<<8 |
                (unsigned)reconstructed[2]<<16 | (unsigned)reconstructed[3]<<24;
            assert(value == ++applied);
        }
        chidmessage_from_remote__free_unpacked(hid, NULL);
        cremote_hidmsg__free_unpacked(remote, NULL);
        IHS_BufferClear(&plain, true);
        IHS_SessionPacketClear(&q->packet, true);
        IHS_QueueItemFree(q);
    }
    assert(applied == 256);
    IHS_SessionDestroy(f.session);
    IHS_Quit();
}
