/* Official client producers 31b480/31ae90/31bd50; protocol reference §114.
 * Capture/codec ownership is external. There is no receive-channel object. */
#include "session_pri.h"
#include "channels/ch_data_microphone.h"
#include "protobuf/pb_utils.h"

#define MICROPHONE_PAYLOAD_LIMIT (1024U * 1024U)

static bool Available(const IHS_Session *s) {
    return !s->destroying && !s->transport.closeRequested && !s->transport.detached &&
        !s->base.interrupted && s->state.connectionState == IHS_SessionConnectionStateConnected;
}

void IHS_SessionMicrophoneClose(IHS_Session *s, bool sendControl) {
    IHS_BaseLock(&s->base);
    uint8_t channel = s->microphone.channel;
    const IHS_StreamMicrophoneCallbacks *callbacks = s->microphone.owner;
    void *context = s->microphone.context;
    s->microphone.channel = 0;
    s->microphone.owner = NULL;
    s->microphone.context = NULL;
    IHS_BaseUnlock(&s->base);
    if (channel)
        IHS_OutgoingPriority(s, channel, 0);
    /* Capture stop/join must never run while holding Base. */
    if (channel && callbacks && callbacks->stop)
        callbacks->stop(s, context);
    if (channel && sendControl) {
        CStopAudioDataMsg message = CSTOP_AUDIO_DATA_MSG__INIT;
        IHS_SessionSendControlMessage(s, k_EStreamControlStopMicrophoneData,
                                      (const ProtobufCMessage *)&message);
    }
}

bool IHS_SessionStartMicrophoneStream(IHS_Session *s, const IHS_StreamAudioConfig *config) {
    if (!s || !config || (!config->codecData && config->codecDataLen) ||
        config->codecDataLen > MICROPHONE_PAYLOAD_LIMIT || !config->frequency || !config->channels)
        return false;
    IHS_BaseLock(&s->base);
    if (!Available(s) || !s->negotiatedConfig ||
        (int)config->codec != (int)s->negotiatedConfig->selected_audio_codec) {
        IHS_BaseUnlock(&s->base);
        return false;
    }
    const IHS_StreamMicrophoneCallbacks *old = s->microphone.owner;
    void *oldContext = s->microphone.context;
    bool replacing = s->microphone.channel != 0;
    /* Original repeated Start replaces the mapping without a Stop message or
     * retiring the old channel's priority. Local capture ownership still ends. */
    s->microphone.channel = 0;
    s->microphone.owner = NULL;
    s->microphone.context = NULL;
    const IHS_StreamMicrophoneCallbacks *callbacks = s->callbacks.microphone;
    void *context = s->callbackContexts.microphone;
    IHS_BaseUnlock(&s->base);
    if (replacing && old && old->stop)
        old->stop(s, oldContext);
    if (callbacks && callbacks->start && callbacks->start(s, config, context) != 0)
        return false;
    IHS_BaseLock(&s->base);
    if (!Available(s)) {
        IHS_BaseUnlock(&s->base);
        if (callbacks && callbacks->stop)
            callbacks->stop(s, context);
        return false;
    }
    uint8_t channel = s->microphone.nextChannel;
    if (channel < 1 || channel > 31)
        channel = 3;
    s->microphone.nextChannel = channel + 1;
    s->microphone.channel = channel;
    s->microphone.nextFrame = 0;
    s->microphone.reliable = !!s->negotiatedConfig->reliable_data;
    s->microphone.owner = callbacks;
    s->microphone.context = context;
    CStartAudioDataMsg message = CSTART_AUDIO_DATA_MSG__INIT;
    message.channel = channel;
    PROTOBUF_C_SET_VALUE(message, codec, (EStreamAudioCodec)config->codec);
    PROTOBUF_C_SET_VALUE(message, frequency, config->frequency);
    PROTOBUF_C_SET_VALUE(message, channels, config->channels);
    if (config->codecData) {
        message.has_codec_data = true;
        message.codec_data.data = config->codecData;
        message.codec_data.len = config->codecDataLen;
    }
    IHS_BaseUnlock(&s->base);
    /* A failed control preparation may synchronously close the session and
     * stop capture. Do not carry Base into that callback/producer join. Data
     * submission starts only after this owner-thread API returns. */
    bool sent = IHS_SessionSendControlMessage(s, k_EStreamControlStartMicrophoneData,
                                             (const ProtobufCMessage *)&message);
    IHS_BaseLock(&s->base);
    if (s->microphone.channel == channel && Available(s))
        IHS_OutgoingPriority(s, channel, 1);
    IHS_BaseUnlock(&s->base);
    return sent; /* Queue admission, not a host/device acknowledgement. */
}

bool IHS_SessionStopMicrophoneStream(IHS_Session *s) {
    if (!s)
        return false;
    IHS_BaseLock(&s->base);
    bool active = s->microphone.channel != 0;
    IHS_BaseUnlock(&s->base);
    IHS_SessionMicrophoneClose(s, true);
    return active;
}

bool IHS_SessionSendMicrophoneData(IHS_Session *s, const uint8_t *data, size_t len) {
    if (!s || (!data && len) || len > MICROPHONE_PAYLOAD_LIMIT)
        return false;
    IHS_BaseLock(&s->base);
    bool sent = false;
    if (s->microphone.channel && !s->destroying) {
        uint16_t frame = s->microphone.nextFrame++;
        sent = IHS_SessionMicrophoneSendFrame(s, s->microphone.channel, frame, data, len,
                                             s->microphone.reliable);
    }
    IHS_BaseUnlock(&s->base);
    return sent;
}
