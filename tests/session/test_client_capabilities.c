#include "ihs_buffer.h"
#include "ihs_buffer_ext.h"
#include "ihslib.h"
#include "session/channels/ch_control.h"
#include "test_session.h"
#include <assert.h>
#include <limits.h>
#include <string.h>

struct IHS_QueueItem {
    IHS_SessionPacket packet;
    bool reliable;
};
static void wire_capabilities(IHS_StreamDeviceFormFactor form, unsigned decode, unsigned burst) {
    IHS_Session *s = IHS_TestSessionCreate();
    IHS_TimerTaskStopImmediate(s->retransmission.timer);
    IHS_TimerTaskStopImmediate(((IHS_SessionChannelControl *)s->channels[1])->feedbackTimer);
    char system[] = "\"SystemInfo\" { \"CPUID\" \"test platform\" }";
    char decoder[] = "test decoder";
    IHS_StreamClientCapabilities requested = {
        .systemInfo = system,
        .decoderInfo = decoder,
        .decoderThreads = 2,
        .maximumDecodeBitrateKbps = decode,
        .maximumBurstBitrateKbps = burst,
        .formFactor = form,
        .hasSystemCanSuspend = true,
        .systemCanSuspend = false,
    };
    assert(IHS_SessionSetClientCapabilities(s, &requested));
    assert(IHS_SessionSetClientCapabilities(s, NULL));
    assert(IHS_SessionSetClientCapabilities(s, &requested));
    memset(system, 'x', sizeof(system) - 1);
    memset(decoder, 'x', sizeof(decoder) - 1);
    IHS_StreamClientCapabilities invalid = {.maximumDecodeBitrateKbps = UINT_MAX};
    assert(!IHS_SessionSetClientCapabilities(s, &invalid));
    invalid = (IHS_StreamClientCapabilities){.formFactor = (IHS_StreamDeviceFormFactor)99};
    assert(!IHS_SessionSetClientCapabilities(s, &invalid));
    s->state.connectionState = IHS_SessionConnectionStateNegotiating;
    assert(!IHS_SessionSetClientCapabilities(s, NULL));
    CNegotiationInitMsg message = CNEGOTIATION_INIT_MSG__INIT;
    EStreamVideoCodec video = k_EStreamVideoCodecH264;
    message.n_supported_video_codecs = 1;
    message.supported_video_codecs = &video;
    IHS_Buffer body = IHS_BUFFER_INIT(1024, 1024);
    IHS_BufferAppendMessage(&body, (ProtobufCMessage *)&message);
    IHS_SessionPacketHeader header = {0};
    IHS_SessionChannelControlOnNegotiation(s->channels[1], k_EStreamControlNegotiationInit, &body,
                                           &header);
    IHS_QueueItem *q = IHS_QueuePoll(s->sendQueue);
    assert(q && q->packet.header.fragmentId == 0);
    assert(*IHS_BufferPointer(&q->packet.body) == k_EStreamControlNegotiationSetConfig);
    IHS_BufferOffsetBy(&q->packet.body, 1);
    IHS_Buffer plain = IHS_BUFFER_INIT(2048, 2048);
    uint64_t sequence;
    assert(IHS_SessionFrameDecrypt(s, &q->packet.body, &plain, 0, &sequence) == 0);
    CNegotiationSetConfigMsg *config =
        cnegotiation_set_config_msg__unpack(NULL, plain.size, IHS_BufferPointer(&plain));
    assert(config && config->streaming_client_caps);
    CStreamingClientCaps *caps = config->streaming_client_caps;
    assert(caps->system_info && strstr(caps->system_info, "test platform"));
    assert(caps->has_system_can_suspend && !caps->system_can_suspend);
    assert(caps->has_form_factor == (form != IHS_StreamDeviceFormFactorUnknown));
    assert((int)caps->form_factor == (int)form);
    assert(caps->has_maximum_decode_bitrate_kbps == (decode != 0));
    assert(caps->has_maximum_burst_bitrate_kbps == (burst != 0));
    assert(caps->maximum_decode_bitrate_kbps == (int)decode);
    assert(caps->maximum_burst_bitrate_kbps == (int)burst);
    cnegotiation_set_config_msg__free_unpacked(config, NULL);
    IHS_BufferClear(&plain, true);
    IHS_BufferClear(&body, true);
    IHS_SessionPacketClear(&q->packet, true);
    IHS_QueueItemFree(q);
    IHS_SessionDestroy(s);
}
int main(void) {
    assert(!IHS_SessionSetClientCapabilities(NULL, NULL));
    IHS_Init();
    wire_capabilities(IHS_StreamDeviceFormFactorComputer, 0, 0);
    wire_capabilities(IHS_StreamDeviceFormFactorUnknown, 30000, 90000);
    IHS_Quit();
    return 0;
}
