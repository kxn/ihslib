/*
 *  _____  _   _  _____  _  _  _     
 * |_   _|| | | |/  ___|| |(_)| |     Steam    
 *   | |  | |_| |\ `--. | | _ | |__     In-Home
 *   | |  |  _  | `--. \| || || '_ \      Streaming
 *  _| |_ | | | |/\__/ /| || || |_) |       Library
 *  \___/ \_| |_/\____/ |_||_||_.__/
 *
 * Copyright (c) 2022 Mariotaku <https://github.com/mariotaku>.
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <stdlib.h>
#include <stdio.h>

#include "ch_control.h"
#include "../frame_stats.h"

#include "session/session_pri.h"

#include "protobuf/pb_utils.h"
#include "protobuf/discovery.pb-c.h"

static void OnNegotiationInit(IHS_SessionChannel *channel, const CNegotiationInitMsg *message, uint16_t packetId);

static void OnNegotiationSetConfig(IHS_SessionChannel *channel, const CNegotiationSetConfigMsg *message,
                                   uint16_t packetId);

/* Generated C protobuf has no CopyFrom: round-trip the complete value so
 * presence, repeated/nested values and unknown fields survive replacement. */
static bool StoreNegotiatedConfig(IHS_Session *session, const CNegotiatedConfig *config) {
    size_t size = cnegotiated_config__get_packed_size(config);
    uint8_t *wire = malloc(size ? size : 1);
    if (!wire)
        return false;
    cnegotiated_config__pack(config, wire);
    CNegotiatedConfig *copy = cnegotiated_config__unpack(NULL, size, wire);
    free(wire);
    if (!copy)
        return false;
    if (session->negotiatedConfig)
        cnegotiated_config__free_unpacked(session->negotiatedConfig, NULL);
    session->negotiatedConfig = copy;
    session->negotiatedVideoCodec = copy->selected_video_codec;
    session->negotiationRemoteHid = !!copy->enable_remote_hid;
    session->negotiationTouchInput = !!copy->enable_touch_input;
    return true;
}

void IHS_SessionChannelControlOnNegotiation(IHS_SessionChannel *channel, EStreamControlMessage type,
                                            IHS_Buffer *payload, const IHS_SessionPacketHeader *header) {
    // Negotiation messages are only valid in the Negotiating phase. Drop replays that
    // arrive before auth succeeds or after we're Connected.
    if (channel->session->state.connectionState != IHS_SessionConnectionStateNegotiating) {
        return;
    }
    switch (type) {
        case k_EStreamControlNegotiationInit: {
            CNegotiationInitMsg *message = IHS_UNPACK_BUFFER(cnegotiation_init_msg__unpack, payload);
            if (message == NULL) {
                IHS_SessionLog(channel->session, IHS_LogLevelWarn, "Control", "Malformed CNegotiationInitMsg");
                break;
            }
            OnNegotiationInit(channel, message, header->packetId);
            cnegotiation_init_msg__free_unpacked(message, NULL);
            break;
        }
        case k_EStreamControlNegotiationSetConfig: {
            CNegotiationSetConfigMsg *message = IHS_UNPACK_BUFFER(cnegotiation_set_config_msg__unpack, payload);
            if (message == NULL) {
                IHS_SessionLog(channel->session, IHS_LogLevelWarn, "Control", "Malformed CNegotiationSetConfigMsg");
                break;
            }
            OnNegotiationSetConfig(channel, message, header->packetId);
            cnegotiation_set_config_msg__free_unpacked(message, NULL);
            break;
        }
        default: {
            abort();
        }
    }
}

static void OnNegotiationInit(IHS_SessionChannel *channel, const CNegotiationInitMsg *message, uint16_t packetId) {
    IHS_Session *session = channel->session;
    IHS_BaseLock(&session->base);
    bool complete = session->clientConfig != NULL;
    CStreamingClientConfig *retained = IHS_SessionClientConfigCopy(session);
    IHS_BaseUnlock(&session->base);
    if (!retained) {
        IHS_SessionDisconnect(session);
        return;
    }
    /* An explicitly supplied complete value includes all optional defaults.
     * Only unconfigured legacy callers receive the convenience defaults below. */
    CStreamingClientConfig clientConfig = *retained;
    /* Official removes the per-session runtime limit before copying SetConfig.
     * The retained clone still owns this pointer and frees it after sending. */
    clientConfig.runtime_limit = NULL;
    uint32_t denominator = clientConfig.desired_framerate_denominator;
    uint32_t fps = clientConfig.desired_framerate_numerator;
    if (fps && denominator)
        fps = ((uint64_t)fps + denominator / 2) / denominator;
    IHS_SessionConfig ihsConf = {
            .enableAudio = clientConfig.enable_audio_streaming,
            .enableHevc = clientConfig.enable_video_hevc,
            .enableUnreliableFec = clientConfig.enable_unreliable_fec,
            .maxWidth = clientConfig.desired_resolution_x ? clientConfig.desired_resolution_x : 1920,
            .maxHeight = clientConfig.desired_resolution_y ? clientConfig.desired_resolution_y : 1080,
            .maxFps = fps ? fps : 60,
            .maxBitrateKbps = clientConfig.desired_bitrate_kbps > 0 ?
                (uint32_t)clientConfig.desired_bitrate_kbps : 15000,
            .quality = (IHS_StreamQuality)clientConfig.quality,
    };
    const IHS_SessionConfig before = ihsConf;

    if (session->callbacks.session && session->callbacks.session->configuring) {
        session->callbacks.session->configuring(session, &ihsConf, session->callbackContexts.session);
    }
    if (ihsConf.maxWidth == 0 || ihsConf.maxHeight == 0) {
        ihsConf.maxWidth = 1920;
        ihsConf.maxHeight = 1080;
    }
    if (ihsConf.maxFps == 0) {
        ihsConf.maxFps = 60;
    }
    if (ihsConf.maxBitrateKbps == 0) {
        ihsConf.maxBitrateKbps = 15000;
    }
    EStreamQualityPreference quality = k_EStreamQualityBalanced;
    switch (ihsConf.quality) {
        case IHS_StreamQualityFast: quality = k_EStreamQualityFast; break;
        case IHS_StreamQualityBeautiful: quality = k_EStreamQualityBeautiful; break;
        default: break; /* Default, Balanced and invalid caller values. */
    }
    IHS_SessionLog(session, IHS_LogLevelInfo, "Negotiation", "Encoding quality preference=%d", quality);
    IHS_SessionLog(session, IHS_LogLevelInfo, "Negotiation",
                   "Host init offers: reliable_data=%d supports_remote_hid=%d supports_touch_input=%d",
                   message->has_reliable_data ? (int) message->reliable_data : -1,
                   message->has_supports_remote_hid ? (int) message->supports_remote_hid : -1,
                   message->has_supports_touch_input ? (int) message->supports_touch_input : -1);

    for (int i = 0; i < message->n_supported_video_codecs; i++) {
        IHS_SessionLog(session, IHS_LogLevelInfo, "Negotiation", "Host offers video codec %d%s",
                       message->supported_video_codecs[i],
                       message->supported_video_codecs[i] == k_EStreamVideoCodecHEVC ? " (HEVC)" :
                       message->supported_video_codecs[i] == k_EStreamVideoCodecH264 ? " (H264)" : "");
    }

    const IHS_StreamClientCapabilities *caps = &session->clientCapabilities;
    bool hevcAvailable = caps->hasVideoDecoderCapabilities ? caps->supportsVideoHevc : ihsConf.enableHevc;
    EStreamAudioCodec audioCodec = k_EStreamAudioCodecNone;
    for (size_t i = 0; i < message->n_supported_audio_codecs; ++i) {
        EStreamAudioCodec codec = message->supported_audio_codecs[i];
        if (codec == k_EStreamAudioCodecRaw ||
            (codec == k_EStreamAudioCodecOpus && !clientConfig.enable_audio_uncompressed)) {
            audioCodec = codec;
            break;
        }
    }
    EStreamVideoCodec videoCodec = k_EStreamVideoCodecNone;
    for (size_t i = 0; i < message->n_supported_video_codecs; ++i) {
        EStreamVideoCodec codec = message->supported_video_codecs[i];
        if (codec == k_EStreamVideoCodecH264 || (codec == k_EStreamVideoCodecHEVC && hevcAvailable)) {
            videoCodec = codec;
            break;
        }
    }
    if (message->has_os_type)
        session->hostOsType = message->os_type;
    if (message->has_gaming_device_type)
        session->hostGamingDeviceType = message->gaming_device_type;

    /* Retain the negotiated preference for diagnostics. StartVideoData supplies
     * the actual decoder configuration; early media waits for that message. */
    session->negotiatedVideoCodec = videoCodec;
    session->captureWidth = 1920;
    session->captureHeight = 1080;

    CNegotiatedConfig config = CNEGOTIATED_CONFIG__INIT;
    /* Original always emits all three scalar presences, even None/false. */
    PROTOBUF_C_SET_VALUE(config, reliable_data, !!message->reliable_data);
    PROTOBUF_C_SET_VALUE(config, selected_audio_codec, audioCodec);
    PROTOBUF_C_SET_VALUE(config, selected_video_codec, videoCodec);

    /* Raw video/AV1/Pyrowave are not supported by this platform pipeline. */
    if (message->supports_remote_hid)
        session->negotiationRemoteHid = true;
    if (message->supports_touch_input)
        session->negotiationTouchInput = true;
    if (session->negotiationRemoteHid)
        PROTOBUF_C_SET_VALUE(config, enable_remote_hid, true);
    if (session->negotiationTouchInput)
        PROTOBUF_C_SET_VALUE(config, enable_touch_input, true);

    /* Official Default omits field 25 (false). Explicit enabled settings and
     * presets request true before the video channel starts; never infer this
     * choice from quality alone or the local ability to receive FEC. */
    if (ihsConf.enableUnreliableFec != clientConfig.enable_unreliable_fec) {
        PROTOBUF_C_SET_VALUE(clientConfig, enable_unreliable_fec, ihsConf.enableUnreliableFec);
    }

    if ((!complete && !clientConfig.has_desired_resolution_x) || ihsConf.maxWidth != before.maxWidth)
        PROTOBUF_C_SET_VALUE(clientConfig, desired_resolution_x, ihsConf.maxWidth);
    if ((!complete && !clientConfig.has_desired_resolution_y) || ihsConf.maxHeight != before.maxHeight)
        PROTOBUF_C_SET_VALUE(clientConfig, desired_resolution_y, ihsConf.maxHeight);
    if (!complete && !clientConfig.has_enable_input_streaming)
        PROTOBUF_C_SET_VALUE(clientConfig, enable_input_streaming, true);
    if (!complete && !clientConfig.has_enable_hardware_decoding)
        PROTOBUF_C_SET_VALUE(clientConfig, enable_hardware_decoding, true);
    if (!complete && !clientConfig.has_enable_performance_overlay)
        PROTOBUF_C_SET_VALUE(clientConfig, enable_performance_overlay, true);
    if (ihsConf.enableAudio) {
        if (!complete && !clientConfig.has_audio_channels)
            PROTOBUF_C_SET_VALUE(clientConfig, audio_channels, 2);
    }
    if ((!complete && !clientConfig.has_enable_audio_streaming) || ihsConf.enableAudio != before.enableAudio)
        PROTOBUF_C_SET_VALUE(clientConfig, enable_audio_streaming, ihsConf.enableAudio);
    // Mic streaming is opt-in: only advertise when the user has registered mic callbacks.
    // Steam's server keys off this flag — without it the server never issues Start, so
    // there's no point in advertising mic support we can't actually fulfil. Matches the
    // capability semantics around CStreamingClientConfig.enable_microphone_streaming
    // documented in the mic RE.
    if (session->callbacks.microphone != NULL) {
        PROTOBUF_C_SET_VALUE(clientConfig, enable_microphone_streaming, true);
    }
    if (!complete && !clientConfig.has_enable_video_streaming)
        PROTOBUF_C_SET_VALUE(clientConfig, enable_video_streaming, true);
    if ((!complete && !clientConfig.has_desired_framerate_numerator) || ihsConf.maxFps != before.maxFps) {
        PROTOBUF_C_SET_VALUE(clientConfig, desired_framerate_numerator, ihsConf.maxFps);
        PROTOBUF_C_SET_VALUE(clientConfig, desired_framerate_denominator, 1);
    }
    if ((!complete && !clientConfig.has_quality) || ihsConf.quality != before.quality)
        PROTOBUF_C_SET_VALUE(clientConfig, quality, quality);
    if ((!complete && !clientConfig.has_desired_bitrate_kbps) || ihsConf.maxBitrateKbps != before.maxBitrateKbps)
        PROTOBUF_C_SET_VALUE(clientConfig, desired_bitrate_kbps, ihsConf.maxBitrateKbps);
    if (ihsConf.enableHevc != clientConfig.enable_video_hevc) {
        PROTOBUF_C_SET_VALUE(clientConfig, enable_video_hevc, ihsConf.enableHevc);
    }

    if (!complete && !clientConfig.controller_overlay_hotkey)
        clientConfig.controller_overlay_hotkey = "auto";

    IHS_SessionLog(session, IHS_LogLevelInfo, "Negotiation",
                   "Request config: %ux%u @ %u/%u fps, %d kbps%s%s",
                   clientConfig.desired_resolution_x, clientConfig.desired_resolution_y,
                   clientConfig.desired_framerate_numerator, clientConfig.desired_framerate_denominator,
                   clientConfig.desired_bitrate_kbps,
                   clientConfig.enable_video_hevc ? ", HEVC" : "",
                   clientConfig.enable_audio_streaming ? ", audio" : "");

    CStreamingClientCaps clientCaps = CSTREAMING_CLIENT_CAPS__INIT;

    clientCaps.system_info = (char *)caps->systemInfo;
    if (caps->hasSystemCanSuspend)
        PROTOBUF_C_SET_VALUE(clientCaps, system_can_suspend, caps->systemCanSuspend);
    if (caps->maximumDecodeBitrateKbps)
        PROTOBUF_C_SET_VALUE(clientCaps, maximum_decode_bitrate_kbps, caps->maximumDecodeBitrateKbps);
    if (caps->maximumBurstBitrateKbps)
        PROTOBUF_C_SET_VALUE(clientCaps, maximum_burst_bitrate_kbps, caps->maximumBurstBitrateKbps);
    if (caps->formFactor != IHS_StreamDeviceFormFactorUnknown)
        PROTOBUF_C_SET_VALUE(clientCaps, form_factor, (EStreamDeviceFormFactor)caps->formFactor);
    /* Preserve official capability order; request preference does not reorder
     * it or make disabled audio decoders unavailable. Raw video is omitted
     * because this library's video pipeline accepts H264/HEVC only. */
    EStreamVideoCodec supportedCodecs[] = {k_EStreamVideoCodecH264, k_EStreamVideoCodecHEVC};
    clientCaps.supported_video_codecs = supportedCodecs;
    clientCaps.n_supported_video_codecs = hevcAvailable ? 2 : 1;
    EStreamAudioCodec supportedAudioCodecs[] = {k_EStreamAudioCodecRaw, k_EStreamAudioCodecOpus};
    clientCaps.supported_audio_codecs = supportedAudioCodecs;
    clientCaps.n_supported_audio_codecs = clientConfig.enable_audio_uncompressed ? 1 : 2;
    if (hevcAvailable)
        PROTOBUF_C_SET_VALUE(clientCaps, supports_video_hevc_obsolete, true);

    CNegotiationSetConfigMsg response = CNEGOTIATION_SET_CONFIG_MSG__INIT;
    response.config = &config;
    if (!IHS_SessionClientConfigStore(session, &clientConfig) ||
        !StoreNegotiatedConfig(session, &config)) {
        cstreaming_client_config__free_unpacked(retained, NULL);
        IHS_SessionDisconnect(session);
        return;
    }
    response.streaming_client_config = &clientConfig;
    response.streaming_client_caps = &clientCaps;

    IHS_SessionChannelControlSend(channel, k_EStreamControlNegotiationSetConfig,
                                  (const ProtobufCMessage *) &response, IHS_PACKET_ID_NEXT);
    cstreaming_client_config__free_unpacked(retained, NULL);
}

static void OnNegotiationSetConfig(IHS_SessionChannel *channel, const CNegotiationSetConfigMsg *message,
                                   uint16_t packetId) {
    (void)packetId;
    IHS_Session *session = channel->session;
    /* The original's codec assertions are diagnostic in non-debug builds;
     * they do not reject the host's complete config. Protocol §49–50. */
    if (!StoreNegotiatedConfig(session, message->config)) {
        IHS_SessionDisconnect(session);
        return;
    }
    session->state.connectionState = IHS_SessionConnectionStateStarting;
}

void IHS_SessionChannelControlUpdateStage(IHS_SessionChannel *channel, uint32_t now) {
    IHS_Session *session = channel->session;
    switch (session->state.connectionState) {
    case IHS_SessionConnectionStateStarting: {
        IHS_SessionChannelControl *control = (void *)channel;
        /* Publish the live phase and enqueue Complete under the existing
         * producer lock: another HID sender must not overtake Complete. */
        IHS_MutexLock(control->sendLock);
        /* State is live before Complete and the application notification. */
        session->state.connectionState = IHS_SessionConnectionStateConnected;
        IHS_OutgoingPriority(session, 1, 2);
        IHS_OutgoingPriority(session, 2, -1);
        control->keepAliveTimestamp = now;
        control->frameEventsTimestamp = now;
        IHS_SendAdmission result = IHS_SessionChannelControlCompleteLocked(control);
        IHS_MutexUnlock(control->sendLock);
        /* Closing may stop workers that also send control. Never do so while
         * holding the producer lock, including the local allocation-failure path. */
        if (result == IHS_SendAdmissionLocalFailure && !session->transport.shutdown &&
            !session->transport.detached)
            IHS_SessionDisconnect(session);
        if (session->callbacks.session && session->callbacks.session->connected)
            session->callbacks.session->connected(session, session->callbackContexts.session);
        break; /* The same update must not also run Streaming. */
    }
    case IHS_SessionConnectionStateConnected: {
        IHS_SessionChannelControl *control = (void *)channel;
        uint32_t last = control->frameEventsTimestamp;
        /* Original HandleStreaming 0x7a717c–0x7a71ac, protocol §55.
         * Match sub/cmn/b.mi, including zero and signed half-domain edges.
         * This consumer must run outside the control producer lock: a local
         * StatsSend failure can synchronously close the session. */
        if (!last || (int32_t)(last - now + 0x10000u) < 0) {
            if (session->frameStats) {
                IHS_SessionSendFrameEvents(session, &session->frameStats->messageScratch);
                if (session->frameTracker)
                    IHS_FrameStatsSettleTracked(session->frameStats, session->frameTracker,
                                               IHS_TimerNow() * 1000);
            }
            control->frameEventsTimestamp = now; /* attempt, even with no frames */
        }
        IHS_SessionChannelControlUpdateHeartbeat(channel, now);
        break;
    }
    default:
        break;
    }
}
