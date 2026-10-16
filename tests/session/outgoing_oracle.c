/* Production cache/sender/socket phases, with controlled clock/final UDP only. */
#include "ihs_buffer.h"
#include "ihs_buffer_ext.h"
#include "session/channels/ch_control.h"
#include "session/session_pri.h"
#include "test_session.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint32_t now;
static unsigned transport;
static char *sent[512];
static unsigned sentCount;
typedef struct {
    unsigned ch, id;
} Seed;
static Seed seeds[256];
static unsigned seedCount;
uint32_t __wrap_IHS_SessionPacketTimestamp(void) {
    return now;
}
bool __wrap_IHS_BaseSend(IHS_Base *b, IHS_SocketAddress addr, const IHS_Buffer *wire) {
    (void)b;
    (void)addr;
    assert(sentCount < 512);
    char *str = malloc(wire->size * 2 + 1);
    assert(str);
    for (size_t i = 0; i < wire->size; ++i)
        sprintf(str + 2 * i, "%02x", IHS_BufferPointer(wire)[i]);
    sent[sentCount++] = str;
    errno = transport == 1 ? EAGAIN : transport == 2 ? EMSGSIZE : transport == 3 ? EBADF : EIO;
    return transport == 0;
}
static bool send_reliable(IHS_SessionPacket *p, void *ctx) {
    return IHS_SessionSendPacket(ctx, p);
}
static void print(IHS_Session *s, int result) {
    printf("{\"sent\":[");
    for (unsigned i = 0; i < sentCount; ++i) {
        printf("%s\"%s\"", i ? "," : "", sent[i]);
        free(sent[i]);
    }
    sentCount = 0;
    printf("],\"unreliable_deadline\":%u,\"reliable_deadline\":%u,\"rotation\":%u,\"blocked\":%s,"
           "\"windows\":[",
           s->outgoing.deadline[0], s->outgoing.deadline[1], s->outgoing.rotation,
           s->outgoing.blocked ? "true" : "false");
    bool first = true;
    for (unsigned ch = 0; ch < 32; ++ch) {
        if (!s->outgoing.active[ch])
            continue;
        IHS_OutgoingWindow *w = &s->outgoing.unreliable[ch];
        printf("%s{\"channel\":%u,\"head\":%u,\"tail\":%u,\"cursor\":%u,\"packets\":[",
               first ? "" : ",", ch, w->head, w->tail, w->cursor);
        first = false;
        bool firstPacket = true;
        for (unsigned i = 0; i < seedCount; ++i) {
            if (seeds[i].ch != ch)
                continue;
            unsigned id = seeds[i].id;
            IHS_OutgoingPacket *p =
                w->capacity && (uint16_t)(id - w->head) < (uint16_t)(w->tail - w->head)
                    ? w->slots[id & (w->capacity - 1)]
                    : NULL;
            printf("%s{\"id\":%u,\"retained\":%s,\"count\":%u,\"stamp\":%u}",
                   firstPacket ? "" : ",", id, p ? "true" : "false",
                   p ? p->packet.header.retransmitCount : 0,
                   p ? p->packet.header.sendTimestamp : 0);
            firstPacket = false;
        }
        printf("]}");
    }
    printf("],\"order\":[");
    for (unsigned i = 0; i < s->outgoing.count; ++i)
        printf("%s%u", i ? "," : "", s->outgoing.order[i]);
    printf("],\"result\":%d}\n", result);
}
int main(void) {
    IHS_Init();
    IHS_Session *s = IHS_TestSessionCreate();
    IHS_TimerTaskStopImmediate(s->retransmission.timer);
    IHS_OutgoingDeinit(s);
    IHS_OutgoingInit(s, 1000000);
    s->state.connectionId = s->state.hostConnectionId = 0;
    s->state.crcRequired = true;
    s->state.connectionState = IHS_SessionConnectionStateConnected;
    s->clock.rttSum = 16384;
    s->clock.rttSamples = 25;
    s->clock.smoothedRTT = 100;
    char line[4096], bitmap[2048];
    unsigned ch, id, count, stamp, created, type, retry, deadline, rate, head, cursor, base,
        echoAge;
    int priority;
    while (fgets(line, sizeof(line), stdin)) {
        if (sscanf(line, "INIT %u %u %u %u", &now, &rate, &deadline, &transport) == 4) {
            s->outgoing.rate[0] = s->outgoing.rate[1] = (int32_t)rate;
            s->outgoing.deadline[0] = s->outgoing.deadline[1] = deadline;
        } else if (sscanf(line, "GET %u", &ch) == 1) {
            IHS_OutgoingRegister(s, ch);
            print(s, 0);
        } else if (sscanf(line, "PRIO %u %d", &ch, &priority) == 2) {
            IHS_OutgoingPriority(s, ch, priority);
        } else if (sscanf(line, "CH %u %d %u %u", &ch, &priority, &head, &cursor) == 4) {
            /* Exact component prestate; actual creation is tested through GET. */
            assert(ch < 32 && !s->outgoing.active[ch]);
            s->outgoing.active[ch] = true;
            s->outgoing.creation[s->outgoing.count] = ch;
            s->outgoing.order[s->outgoing.count++] = ch;
            s->outgoing.priority[ch] = priority;
            s->outgoing.dirty = true;
            IHS_OutgoingWindow *w = &s->outgoing.unreliable[ch];
            w->head = w->tail = head;
            w->cursor = cursor;
            w->initialized = true;
        } else if (sscanf(line, "PK %u %u %u %u %u %u", &type, &ch, &id, &count, &stamp,
                          &created) == 6) {
            IHS_SessionPacket p = {0};
            p.header.type = type;
            p.header.channelId = ch;
            p.header.packetId = id;
            p.header.retransmitCount = count;
            p.header.sendTimestamp = stamp;
            IHS_SessionPacketBodyInitialize(&p.body, false);
            IHS_BufferAppendUInt8(&p.body, ch);
            if (type == 3) {
                assert(IHS_OutgoingRetain(s, &p, created));
                assert(seedCount < 256);
                seeds[seedCount++] = (Seed){ch, id};
            } else {
                p.header.retransmitCount = count ? count - 1 : 0;
                assert(IHS_RetransmissionTrack(&s->retransmission, &p, 0));
                IHS_RetransmissionNoteInitialSend(&s->retransmission, &p.header, count != 0, 0);
            }
            IHS_SessionPacketClear(&p, true);
        } else if (sscanf(line, "TIME %u %u", &now, &transport) == 2) {
        } else if (sscanf(line, "SENDU %u %u", &ch, &retry) == 2) {
            print(s, IHS_OutgoingSendUnreliable(s, ch, retry));
        } else if (sscanf(line, "SENDR %u", &ch) == 1) {
            IHS_RetransmissionProcessChannel(&s->retransmission, 0, now, ch, send_reliable, s);
            print(s, 0);
        } else if (!strncmp(line, "UPDATE", 6)) {
            IHS_OutgoingUpdate(s, now);
            print(s, 0);
        } else if (sscanf(line, "NACKRAW %u %u %u %2047s", &ch, &base, &stamp, bitmap) == 4) {
            IHS_SessionPacket p = {0};
            p.header.type = 10;
            p.header.channelId = ch;
            p.header.packetId = base;
            p.header.sendTimestamp = stamp;
            p.header.hasCrc = true;
            IHS_SessionPacketBodyInitialize(&p.body, true);
            if (strcmp(bitmap, "empty"))
                for (size_t i = 0; i < strlen(bitmap) / 2; ++i) {
                    unsigned byte;
                    assert(sscanf(bitmap + 2 * i, "%2x", &byte) == 1);
                    IHS_BufferAppendUInt8(&p.body, byte);
                }
            IHS_SessionPacketPopulateBuffer(&p);
            IHS_Buffer raw = {0};
            IHS_BufferTransferOwnership(&p.body, &raw);
            IHS_BufferExtendSize(&raw);
            s->base.callbacks.received(&s->base, &s->info.address, &raw);
            IHS_BufferClear(&raw, true);
            IHS_SessionPacketClear(&p, true);
            print(s, 0);
        } else if (sscanf(line, "NACK %u %u %u %u %2047s", &ch, &base, &stamp, &echoAge, bitmap) ==
                   5) {
            IHS_SessionPacket p = {0};
            p.header.type = 10;
            p.header.channelId = ch;
            p.header.packetId = base;
            p.header.sendTimestamp = stamp;
            p.header.hasCrc = true;
            IHS_SessionPacketBodyInitialize(&p.body, true);
            if (strcmp(bitmap, "none")) {
                IHS_BufferAppendUInt32LE(&p.body, now - echoAge);
                IHS_BufferAppendUInt16LE(&p.body, (uint16_t)(s->outgoing.unreliable[ch].head - 1));
                if (strcmp(bitmap, "empty"))
                    for (size_t i = 0; i < strlen(bitmap) / 2; ++i) {
                        unsigned byte;
                        assert(sscanf(bitmap + 2 * i, "%2x", &byte) == 1);
                        IHS_BufferAppendUInt8(&p.body, byte);
                    }
            }
            IHS_SessionPacketPopulateBuffer(&p);
            IHS_Buffer raw = {0};
            IHS_BufferTransferOwnership(&p.body, &raw);
            IHS_BufferExtendSize(&raw);
            s->base.callbacks.received(&s->base, &s->info.address, &raw);
            IHS_BufferClear(&raw, true);
            IHS_SessionPacketClear(&p, true);
            print(s, 0);
        } else
            abort();
    }
    IHS_SessionDestroy(s);
    IHS_Quit();
    return 0;
}
