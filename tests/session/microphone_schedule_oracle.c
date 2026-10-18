/* Protocol reference §115: controlled API/dispatch and per-channel scan steps.
 * Clock, crypto identity and final UDP are adapters; no capture or host runs. */
#include "session/session_pri.h"
#include "test_session.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t now = 1000000;
static char *wires[64];
static unsigned count;
uint32_t __wrap_IHS_SessionPacketTimestamp(void) {
    return now;
}
int __wrap_IHS_SessionFrameEncrypt(IHS_Session *s, const uint8_t *in, size_t n, uint8_t *out,
                                   size_t *len, uint64_t sequence) {
    (void)s;
    (void)sequence;
    assert(*len >= n);
    if (n)
        memcpy(out, in, n);
    *len = n;
    return 0;
}
bool __wrap_IHS_BaseSend(IHS_Base *base, IHS_SocketAddress address, const IHS_Buffer *wire) {
    (void)base;
    (void)address;
    assert(count < 64);
    char *hex = malloc(wire->size * 2 + 1);
    assert(hex);
    for (size_t i = 0; i < wire->size; ++i)
        sprintf(hex + 2 * i, "%02x", IHS_BufferPointer(wire)[i]);
    wires[count++] = hex;
    return true;
}
static void observe(IHS_Session *s) {
    printf("{\"now\":%u,\"wires\":[", now);
    for (unsigned i = 0; i < count; ++i) {
        printf("%s\"%s\"", i ? "," : "", wires[i]);
        free(wires[i]);
    }
    count = 0;
    printf("],\"deadlines\":[%u,%u],\"blocked\":%s,\"channel\":%u,\"frame\":%u,\"channels\":[",
           s->outgoing.deadline[0], s->outgoing.deadline[1], s->outgoing.blocked ? "true" : "false",
           s->microphone.channel, s->microphone.nextFrame);
    bool first = true;
    for (unsigned ch = 0; ch < 32; ++ch) {
        if (!s->outgoing.active[ch])
            continue;
        printf("%s[%u,%d,%u,%u]", first ? "" : ",", ch, s->outgoing.priority[ch],
               s->outgoing.reliableId[ch], s->outgoing.unreliableId[ch]);
        first = false;
    }
    puts("]}");
}
int main(int argc, char **argv) {
    assert(argc == 4);
    IHS_Init();
    IHS_Session *s = IHS_TestSessionCreate();
    IHS_TimerTaskStopImmediate(s->retransmission.timer);
    s->state.connectionState = IHS_SessionConnectionStateConnected;
    s->state.connectionId = 12;
    s->state.hostConnectionId = 34;
    s->state.crcRequired = atoi(argv[1]) != 0;
    s->state.mtu = 1472;
    s->outgoing.rate[1] = atoi(argv[3]);
    s->negotiatedConfig = cnegotiated_config__unpack(NULL, 0, NULL);
    assert(s->negotiatedConfig);
    s->negotiatedConfig->selected_audio_codec = k_EStreamAudioCodecOpus;
    s->negotiatedConfig->reliable_data = atoi(argv[2]) != 0;
    const IHS_StreamAudioConfig config = {
        .codec = IHS_StreamAudioCodecOpus, .frequency = 48000, .channels = 2};
    const uint8_t data[] = {0x11, 0x22, 0x33, 0x44};
    char line[128], op[16];
    unsigned channel;
    while (fgets(line, sizeof(line), stdin)) {
        assert(sscanf(line, "%15s %u %u", op, &now, &channel) == 3);
        if (!strcmp(op, "start"))
            assert(IHS_SessionStartMicrophoneStream(s, &config));
        else if (!strcmp(op, "stop"))
            assert(IHS_SessionStopMicrophoneStream(s));
        else if (!strcmp(op, "data"))
            assert(IHS_SessionSendMicrophoneData(s, data, sizeof(data)));
        else {
            assert(!strcmp(op, "scan"));
            IHS_OutgoingUpdateChannel(s, (uint8_t)channel, now);
        }
        while (IHS_SessionDispatchQueued(s, false)) {
        }
        observe(s);
    }
    IHS_SessionDestroy(s);
    IHS_Quit();
    return 0;
}
