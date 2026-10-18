/* Real negotiation producer -> frame batch/retained sender -> Socket -> owned
 * reassembly/decryption. Only the clock and final UDP write are controlled. */
#include "ihs_buffer.h"
#include "session/channels/ch_control.h"
#include "session/session_pri.h"
#include "test_session.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static IHS_Session *session;
static IHS_SessionPacketsWindow *outgoing;
static unsigned frameCount;
static uint64_t sequence;
static char *frames[32];
uint32_t __wrap_IHS_SessionPacketTimestamp(void) { return 1000000; }
static char *hex(const uint8_t *p, size_t n) {
    char *text = malloc(n * 2 + 1);
    assert(text);
    for (size_t i = 0; i < n; ++i)
        sprintf(text + 2 * i, "%02x", p[i]);
    text[2 * n] = 0;
    return text;
}
bool __wrap_IHS_BaseSend(IHS_Base *base, IHS_SocketAddress address, const IHS_Buffer *wire) {
    (void)base;
    (void)address;
    IHS_Buffer copy = IHS_BUFFER_INIT(1024, 0);
    IHS_BufferAppendMem(&copy, IHS_BufferPointer(wire), wire->size);
    IHS_SessionPacket p;
    assert(IHS_SessionPacketParse(&p, &copy) == IHS_SessionPacketResultOK);
    assert(p.header.channelId == 1 && (p.header.type == 5 || p.header.type == 6));
    assert(IHS_SessionPacketsWindowAdd(outgoing, &p));
    IHS_SessionPacketClear(&p, true);
    IHS_BufferClear(&copy, true);
    IHS_SessionFrame f = {.body = IHS_BUFFER_INIT(1024, 0)};
    while (IHS_SessionPacketsWindowPoll(outgoing, &f)) {
        assert(frameCount < 32 && f.body.size > 1);
        unsigned type = *IHS_BufferPointer(&f.body);
        assert(type == 4);
        IHS_BufferOffsetBy(&f.body, 1);
        IHS_Buffer plain = IHS_BUFFER_INIT(1024, 0);
        uint64_t actual;
        assert(IHS_SessionFrameDecrypt(session, &f.body, &plain, sequence++, &actual) ==
               IHS_SessionFrameDecryptOK);
        assert(actual == sequence - 1);
        IHS_Buffer full = IHS_BUFFER_INIT(1024, 0);
        IHS_BufferAppendMem(&full, (uint8_t[]){4}, 1);
        IHS_BufferAppendMem(&full, IHS_BufferPointer(&plain), plain.size);
        frames[frameCount++] = hex(IHS_BufferPointer(&full), full.size);
        IHS_BufferClear(&full, true);
        IHS_BufferClear(&plain, true);
        IHS_SessionFrameClear(&f, true);
        f = (IHS_SessionFrame){.body = IHS_BUFFER_INIT(1024, 0)};
    }
    IHS_SessionFrameClear(&f, true);
    return true;
}
static IHS_Buffer bytes(const char *encoded) {
    IHS_Buffer b = IHS_BUFFER_INIT(1024, 0);
    IHS_BufferEnsureCapacity(&b, 1);
    if (!strcmp(encoded, "-"))
        return b;
    for (size_t i = 0; encoded[i]; i += 2) {
        unsigned v;
        assert(encoded[i + 1] && sscanf(encoded + i, "%2x", &v) == 1);
        uint8_t byte = v;
        IHS_BufferAppendMem(&b, &byte, 1);
    }
    return b;
}
int main(int argc, char **argv) {
    assert(argc == 4);
    IHS_Init();
    session = IHS_TestSessionCreate();
    IHS_TimerTaskStopImmediate(session->retransmission.timer);
    IHS_StreamClientCapabilities caps = {.hasVideoDecoderCapabilities = true,
                                         .supportsVideoHevc = atoi(argv[2]) != 0};
    IHS_Buffer inputCaps = bytes(argv[3]);
    CStreamingClientCaps *initialCaps = cstreaming_client_caps__unpack(NULL, inputCaps.size, IHS_BufferPointer(&inputCaps));
    assert(initialCaps);
    caps.hasLegacyHevcCapability = initialCaps->has_supports_video_hevc_obsolete;
    caps.legacyHevcCapability = initialCaps->supports_video_hevc_obsolete;
    cstreaming_client_caps__free_unpacked(initialCaps, NULL);
    IHS_BufferClear(&inputCaps, true);
    assert(IHS_SessionSetClientCapabilities(session, &caps));
    IHS_Buffer initial = bytes(argv[1]);
    assert(IHS_SessionSetStreamingClientConfig(session, IHS_BufferPointer(&initial), initial.size,
                                               true));
    IHS_BufferClear(&initial, true);
    session->state.connectionState = IHS_SessionConnectionStateNegotiating;
    session->state.connectionId = 12;
    session->state.hostConnectionId = 34;
    session->state.crcRequired = true;
    session->state.mtu = 128;
    session->outgoing.rate[0] = session->outgoing.rate[1] = 0;
    outgoing = IHS_SessionPacketsWindowCreateReliable(16, 0);
    assert(outgoing);
    char line[16384];
    unsigned stage = 0;
    putchar('[');
    while (fgets(line, sizeof(line), stdin)) {
        line[strcspn(line, "\r\n")] = 0;
        IHS_Buffer b = bytes(line);
        IHS_SessionPacketHeader header = {0};
        IHS_SessionChannelControlOnNegotiation(session->channels[1],
                                               k_EStreamControlNegotiationInit, &b, &header);
        IHS_BufferClear(&b, true);
        while (IHS_SessionDispatchQueued(session, false)) {
        }
        if (stage++)
            putchar(',');
        printf("{\"frames\":[");
        for (unsigned i = 0; i < frameCount; ++i) {
            printf("%s\"%s\"", i ? "," : "", frames[i]);
            free(frames[i]);
        }
        frameCount = 0;
        size_t n = IHS_SessionGetStreamingClientConfig(session, NULL, 0);
        uint8_t *data = malloc(n ? n : 1);
        assert(data);
        assert(IHS_SessionGetStreamingClientConfig(session, data, n) == n);
        char *encoded = hex(data, n);
        free(data);
        IHS_SessionChannelControl *control = (void *)session->channels[1];
        printf("],\"client_config\":\"%s\",\"host_os\":%u,\"gaming_device\":%u,\"sequence\":%llu}",
               encoded, (uint32_t)session->hostOsType, (uint32_t)session->hostGamingDeviceType,
               (unsigned long long)control->sendEncryptSequence);
        free(encoded);
    }
    puts("]");
    IHS_SessionPacketsWindowDestroy(outgoing);
    IHS_SessionDestroy(session);
    IHS_Quit();
    return 0;
}
