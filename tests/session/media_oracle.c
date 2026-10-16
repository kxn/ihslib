/* Machine-readable driver for the REAL receiver functions. The Python audit
 * feeds the same stream to original ARM64 instructions; no independent C model. */
#include "session/channels/ch_data.h"
#include "session/session_pri.h"
#include "test_session.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint32_t now = 1000000;
uint32_t __wrap_IHS_SessionPacketTimestamp(void) {
    return now;
}
static void hex(const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++)
        printf("%02x", p[i]);
}
static void frames(IHS_SessionChannelData *ch) {
    IHS_SessionFrame f = {0};
    IHS_BufferInit(&f.body, 32, 1048576);
    for (;;) {
        if (IHS_SessionPacketsWindowPoll(ch->window, &f)) {
            printf("{\"frame\":%u,\"send\":%u,\"payload\":\"", f.header.packetId,
                   f.header.sendTimestamp);
            hex(IHS_BufferPointer(&f.body), f.body.size);
            puts("\"}");
            IHS_SessionPacketsWindowReleaseFrame(&f);
            continue;
        }
        if (!IHS_SessionPacketsWindowDiscard(
                ch->window, IHS_StreamClockMediaDelayTicks(&ch->base.session->clock)))
            break;
    }
    IHS_BufferClear(&f.body, true);
    printf("{\"head\":%u}\n", IHS_SessionPacketsWindowNextNeededPacketId(ch->window));
}
int main(int argc, char **argv) {
    assert(argc == 3);
    IHS_Init();
    IHS_Session *s = IHS_TestSessionCreate();
    s->state.connectionId = 12;
    s->clock.rttSamples = 1;
    s->clock.rttSum = strtoul(argv[1], NULL, 0);
    s->clock.smoothedRTT = strtoul(argv[2], NULL, 0);
    IHS_SessionChannelData ch = {
        .base = {.session = s, .type = IHS_SessionChannelTypeDataVideo, .id = 3}};
    ch.window = IHS_SessionPacketsWindowCreateMedia(2048);
    ch.windowLock = IHS_MutexCreate();
    ch.windowCond = IHS_CondCreate();
    ch.feedbackCond = IHS_CondCreate();
    atomic_init(&ch.interrupted, false);
    char line[256];
    while (fgets(line, sizeof(line), stdin)) {
        unsigned id, stamp, received, count, type, fragment, tick;
        if (sscanf(line, "A %u %u %u %u %u %u", &id, &stamp, &received, &count, &type, &fragment) ==
            6) {
            IHS_SessionPacket p = {.header = {.packetId = id,
                                              .sendTimestamp = stamp,
                                              .receiveTimestamp = received,
                                              .retransmitCount = count,
                                              .type = type,
                                              .fragmentId = fragment}};
            IHS_BufferInit(&p.body, 1, 1);
            uint8_t value = id;
            IHS_BufferAppendMem(&p.body, &value, 1);
            IHS_SessionChannelDataReceived(&ch.base, &p);
            IHS_SessionPacketClear(&p, true);
        } else if (sscanf(line, "F %u", &tick) == 1) {
            now = tick;
            IHS_SessionPacket p;
            uint32_t wait;
            bool ok = IHS_SessionChannelDataBuildFeedback(&ch, now, &p, &wait);
            printf("{\"ready\":%s", ok ? "true" : "false");
            if (ok) {
                p.header.sendTimestamp = now;
                IHS_SessionPacketPopulateBuffer(&p);
                printf(",\"wire\":\"");
                hex(p.body.data, IHS_SessionPacketSize(&p));
                printf("\"");
                IHS_SessionPacketClear(&p, true);
            }
            puts("}");
        } else if (sscanf(line, "S %u", &id) == 1) {
            /* Test a post-resynchronization watermark near 16-bit wrap. */
            IHS_SessionPacketsWindowReleaseAll(ch.window);
        } else if (line[0] == 'U') {
            frames(&ch);
        } else {
            assert(!"bad oracle command");
        }
    }
    IHS_SessionChannelDataDeinit(&ch.base);
    IHS_SessionDestroy(s);
    IHS_Quit();
    return 0;
}
