/* Drive actual session ingress, reliable control reassembly and worker update.
 * Clock, channel consumer and the final disconnect action are controlled;
 * packet CRC/guards, handshake, protobuf and queued message bytes are real. */
#include "ihs_buffer.h"
#include "ihs_buffer_ext.h"
#include "session/channels/ch_control.h"
#include "session/channels/ch_data.h"
#include "session/session_pri.h"
#include "test_session.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    IHS_SessionPacket packet;
    bool reliable;
} QueuedPacket;
static uint32_t now;
static char actions[4096];
static uint64_t encryptedSequence;
static unsigned received;
static bool sentCRC;
static size_t sentSize;
bool __wrap_IHS_BaseSend(IHS_Base *base, IHS_SocketAddress address, const IHS_Buffer *wire) {
    (void)base;
    (void)address;
    IHS_Buffer copy = IHS_BUFFER_INIT(2048, 2048);
    IHS_BufferAppendMem(&copy, IHS_BufferPointer(wire), wire->size);
    IHS_SessionPacket packet;
    assert(IHS_SessionPacketParse(&packet, &copy) == IHS_SessionPacketResultOK);
    sentCRC = packet.header.hasCrc;
    sentSize = wire->size;
    assert(packet.body.size == 3 && !memcmp(IHS_BufferPointer(&packet.body), "crc", 3));
    IHS_SessionPacketClear(&packet, true);
    IHS_BufferClear(&copy, true);
    return true;
}
uint32_t __wrap_IHS_SessionPacketTimestamp(void) {
    return now;
}
void __wrap_IHS_SessionDisconnect(IHS_Session *s) {
    (void)s;
    strcat(actions, "D ");
}
static void channel_received(IHS_SessionChannel *c, IHS_SessionPacket *p) {
    (void)c;
    (void)p;
    received++;
    strcat(actions, "channel ");
}
static const IHS_SessionChannelClass consumer = {.received = channel_received,
                                                 .instanceSize = sizeof(IHS_SessionChannel)};

static void drain(IHS_Session *s) {
    QueuedPacket *queued;
    while ((queued = (void *)IHS_QueuePoll(s->sendQueue))) {
        IHS_SessionPacket *p = &queued->packet;
        if (p->header.type == IHS_SessionPacketTypeReliable) {
            unsigned type = *IHS_BufferPointer(&p->body);
            char name[16];
            snprintf(name, sizeof(name), "C%u ", type);
            strcat(actions, name);
            if (type == k_EStreamControlPause || type == k_EStreamControlResume) {
                IHS_BufferOffsetBy(&p->body, 1);
                IHS_Buffer plain = IHS_BUFFER_INIT(1024, 1024);
                uint64_t sequence;
                assert(IHS_SessionFrameDecrypt(s, &p->body, &plain, encryptedSequence++,
                                               &sequence) == IHS_SessionFrameDecryptOK);
                assert(plain.size == 0);
                IHS_BufferClear(&plain, true);
            }
        } else if (p->header.type == IHS_SessionPacketTypeUnreliable &&
                   (p->header.channelId == 3 || p->header.channelId == 4)) {
            assert(p->body.size == 1 && *IHS_BufferPointer(&p->body) == k_EStreamDataLost);
            strcat(actions, p->header.channelId == 3 ? "L0 " : "L1 ");
        }
        IHS_SessionPacketClear(p, true);
        IHS_QueueItemFree((void *)queued);
    }
}
static void print(IHS_Session *s) {
    drain(s);
    printf("{\"phase\":%d,\"mtu\":%d,\"crc\":%s,\"client_paused\":%s,"
           "\"connection_paused\":%s,\"last_receive\":%u,\"last_echo\":%u,"
           "\"timed_out\":%s,\"host_stop\":%s,\"received\":%u,\"sent_crc\":%s,"
           "\"sent_bytes\":%zu,\"actions\":\"%s\"}\n",
           (int)s->state.connectionState, (int)s->state.mtu,
           s->state.crcRequired ? "true" : "false", s->transport.clientPaused ? "true" : "false",
           s->transport.connectionPaused ? "true" : "false", s->transport.lastReceive,
           s->clock.lastEcho, s->transport.timedOut ? "true" : "false",
           IHS_SessionHostRequestedStop(s) ? "true" : "false", received, sentCRC ? "true" : "false",
           sentSize, actions);
    actions[0] = 0;
}
int main(int argc, char **argv) {
    assert(argc == 6);
    IHS_Init();
    IHS_Session *s = IHS_TestSessionCreate();
    assert(s);
    IHS_TimerTaskStopImmediate(s->retransmission.timer);
    IHS_TimerTaskStopImmediate(((IHS_SessionChannelControl *)s->channels[1])->feedbackTimer);
    s->state.connectionState = atoi(argv[1]);
    s->state.connectionId = 12;
    s->state.hostConnectionId = 34;
    unsigned paused = (unsigned)atoi(argv[2]);
    s->transport.clientPaused = s->transport.connectionPaused = paused;
    s->transport.appliedGeneration = paused;
    atomic_store(&s->transport.lifecycleGeneration, paused);
    s->transport.lastReceive = (uint32_t)strtoul(argv[3], NULL, 0);
    s->clock.lastEcho = (uint32_t)strtoul(argv[4], NULL, 0);
    s->clock.haveEcho = true;
    s->transport.updateTime = (uint32_t)strtoul(argv[5], NULL, 0);
    for (unsigned i = 0; i < 2; ++i)
        IHS_SessionChannelAdd(s, IHS_SessionChannelCreate(&consumer, s,
                                                          i ? IHS_SessionChannelTypeDataVideo
                                                            : IHS_SessionChannelTypeDataAudio,
                                                          3 + i, NULL));
    char line[8192], hex[8192];
    unsigned value, address, peerTime;
    while (fgets(line, sizeof(line), stdin)) {
        if (sscanf(line, "WIRE %u %u %8191s", &now, &address, hex) == 3) {
            size_t length = strlen(hex);
            assert(!(length & 1));
            uint8_t *bytes = malloc(length / 2);
            assert(bytes);
            for (size_t i = 0; i < length / 2; ++i) {
                assert(sscanf(hex + i * 2, "%2x", &value) == 1);
                bytes[i] = (uint8_t)value;
            }
            IHS_Buffer buffer = IHS_BUFFER_WRAP(bytes, length / 2);
            IHS_SocketAddress peer = s->info.address;
            if (!address)
                peer.port++;
            s->base.callbacks.received(&s->base, &peer, &buffer);
            IHS_BufferClear(&buffer, true);
        } else if (sscanf(line, "FOREGROUND %u", &value) == 1) {
            IHS_SessionSetForeground(s, value != 0);
            continue;
        } else if (sscanf(line, "UPDATE %u", &now) == 1) {
            s->base.callbacks.run->update(&s->base, NULL);
        } else if (sscanf(line, "FEEDBACK %u %u %u", &value, &peerTime, &now) == 3) {
            IHS_StreamClockFeedback(&s->clock, value, peerTime, now);
        } else if (sscanf(line, "BEGIN %u", &now) == 1) {
            IHS_SessionTransportBegin(s, now);
            continue;
        } else if (sscanf(line, "MTU %u", &value) == 1) {
            s->state.mtu = (int)value;
            continue;
        } else if (sscanf(line, "FRAGMENT %u", &value) == 1) {
            assert(value > 0 && value < 100000);
            unsigned frameCRC = 1;
            sscanf(line, "FRAGMENT %u %u", &value, &frameCRC);
            IHS_SessionFrame frame;
            assert(IHS_SessionChannelInitializeFrame(s->channels[1], &frame,
                                                     IHS_SessionPacketTypeReliable, frameCRC != 0,
                                                     IHS_PACKET_ID_NEXT));
            /* Fixture owns a larger plaintext frame to exercise fragmentation;
             * production packet buffers retain their normal bounded capacity. */
            frame.body.maxCapacity = 131072;
            IHS_BufferFillMem(&frame.body, 0, 0x5a, value);
            assert(IHS_SessionChannelQueueFrame(s->channels[1], &frame, false));
            IHS_SessionFrameClear(&frame, true);
            QueuedPacket *q;
            unsigned total = 0, packets = 0, maxWire = 0;
            unsigned expectedPackets = 0;
            while ((q = (void *)IHS_QueuePoll(s->sendQueue))) {
                assert(q->packet.header.hasCrc == (frameCRC != 0 || s->state.crcRequired));
                assert(IHS_SessionPacketSize(&q->packet) <= (unsigned)s->state.mtu);
                unsigned size = (unsigned)IHS_SessionPacketSize(&q->packet);
                if (size > maxWire)
                    maxWire = size;
                if (packets)
                    assert(q->packet.header.fragmentId == (int)packets - 1);
                else
                    expectedPackets = q->packet.header.fragmentId + 1;
                for (size_t i = 0; i < q->packet.body.size; ++i)
                    assert(IHS_BufferPointer(&q->packet.body)[i] == 0x5a);
                total += q->packet.body.size;
                packets++;
                IHS_SessionPacketClear(&q->packet, true);
                IHS_QueueItemFree((void *)q);
            }
            assert(total == value && packets == expectedPackets);
            printf("{\"fragments\":%u,\"max_wire\":%u,\"payload\":%u}\n", packets, maxWire, total);
            continue;
        } else if (line[0] == 'S') {
            IHS_SessionPacket packet;
            assert(IHS_SessionChannelInitializePacket(
                s->channels[1], &packet, IHS_SessionPacketTypeUnreliableNack, false, 0));
            IHS_BufferAppendMem(&packet.body, (const uint8_t *)"crc", 3);
            assert(IHS_SessionSendPacket(s, &packet));
            IHS_SessionPacketClear(&packet, true);
        } else {
            assert(!"bad transport oracle command");
        }
        print(s);
    }
    IHS_SessionDestroy(s);
    IHS_Quit();
    return 0;
}
