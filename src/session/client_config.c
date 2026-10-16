/* Official client: SetStreamingClientConfig 0x7b01c4, incoming 0x7af08c.
 * A config is a complete protobuf value, including presence/unknown fields. */
#include "session_pri.h"
#include <stdlib.h>

static const CStreamingClientConfig defaults = CSTREAMING_CLIENT_CONFIG__INIT;

static uint8_t *Pack(const CStreamingClientConfig *config, size_t *size) {
    *size = cstreaming_client_config__get_packed_size(config);
    uint8_t *wire = malloc(*size ? *size : 1);
    if (wire)
        cstreaming_client_config__pack(config, wire);
    return wire;
}

CStreamingClientConfig *IHS_SessionClientConfigCopy(IHS_Session *s) {
    IHS_BaseLock(&s->base);
    size_t size;
    uint8_t *wire = Pack(s->clientConfig ? s->clientConfig : &defaults, &size);
    CStreamingClientConfig *copy = wire ? cstreaming_client_config__unpack(NULL, size, wire) : NULL;
    free(wire);
    IHS_BaseUnlock(&s->base);
    return copy;
}

bool IHS_SessionClientConfigStore(IHS_Session *s, const CStreamingClientConfig *config) {
    size_t size;
    uint8_t *wire = Pack(config, &size);
    if (!wire)
        return false;
    CStreamingClientConfig *copy = cstreaming_client_config__unpack(NULL, size, wire);
    free(wire);
    if (!copy)
        return false;
    IHS_BaseLock(&s->base);
    CStreamingClientConfig *old = s->clientConfig;
    s->clientConfig = copy;
    /* Optional defaults are part of the replacement. Missing is not a patch. */
    s->state.streamingInput = copy->enable_input_streaming;
    s->state.streamingAudio = copy->enable_audio_streaming;
    s->state.streamingVideo = copy->enable_video_streaming;
    if (old)
        cstreaming_client_config__free_unpacked(old, NULL);
    IHS_BaseUnlock(&s->base);
    return true;
}

size_t IHS_SessionGetStreamingClientConfig(IHS_Session *s, uint8_t *wire, size_t capacity) {
    if (!s)
        return 0;
    IHS_BaseLock(&s->base);
    const CStreamingClientConfig *config = s->clientConfig ? s->clientConfig : &defaults;
    size_t size = cstreaming_client_config__get_packed_size(config);
    if (wire && capacity >= size)
        cstreaming_client_config__pack(config, wire);
    IHS_BaseUnlock(&s->base);
    return size;
}

bool IHS_SessionClientConfigApply(IHS_Session *s, const CStreamingClientConfig *config,
                                  bool localOnly) {
    IHS_BaseLock(&s->base);
    if (!IHS_SessionClientConfigStore(s, config)) {
        IHS_BaseUnlock(&s->base);
        return false;
    }
    const IHS_StreamSessionCallbacks *callbacks = s->callbacks.session;
    if (callbacks && callbacks->clientConfigChanged) {
        size_t size;
        uint8_t *wire = Pack(s->clientConfig, &size);
        if (wire) {
            callbacks->clientConfigChanged(s, wire, size, localOnly, s->callbackContexts.session);
            free(wire);
        }
    }
    /* A delegate may have applied another config recursively (official player
     * microphone handling does this). Save the current value after it returns. */
    if (callbacks && callbacks->saveClientConfig) {
        size_t size;
        uint8_t *wire = Pack(s->clientConfig, &size);
        if (wire) {
            callbacks->saveClientConfig(s, wire, size, s->callbackContexts.session);
            free(wire);
        }
    }
    bool result = true;
    if (!localOnly && s->state.connectionState == IHS_SessionConnectionStateConnected) {
        CSetStreamingClientConfig message = CSET_STREAMING_CLIENT_CONFIG__INIT;
        /* The original setter sends its input, which need not alias the
         * stored config modified by a recursive delegate. */
        message.config = (CStreamingClientConfig *)config;
        result = IHS_SessionSendControlMessage(s, k_EStreamControlSetStreamingClientConfig,
                                               (const ProtobufCMessage *)&message);
    }
    IHS_BaseUnlock(&s->base);
    return result;
}

bool IHS_SessionSetStreamingClientConfig(IHS_Session *s, const uint8_t *wire, size_t size,
                                         bool localOnly) {
    if (!s || (!wire && size))
        return false;
    CStreamingClientConfig *config = cstreaming_client_config__unpack(NULL, size, wire);
    if (!config)
        return false;
    bool result = IHS_SessionClientConfigApply(s, config, localOnly);
    cstreaming_client_config__free_unpacked(config, NULL);
    return result;
}
