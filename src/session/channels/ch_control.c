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

#include "ch_control.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "crypto.h"
#include "session/frame.h"
#include "session/window.h"
#include "ch_discovery.h"

#include "session/session_pri.h"
#include "protobuf/pb_utils.h"

#include "ihs_buffer_ext.h"

static bool IsMessageEncrypted(EStreamControlMessage type);

static size_t EncryptedMessageCapacity(size_t plainSize);

static void OnControlInit(IHS_SessionChannel *channel, const void *data);
static void ControlOnNackPacket(IHS_SessionChannelControl *control, IHS_SessionPacket *packet);
static void ControlSendGapNack(IHS_SessionChannelControl *control);

static void OnControlDeinit(IHS_SessionChannel *channel);

static void OnControlReceived(IHS_SessionChannel *channel, IHS_SessionPacket *packet);

/* Defined non-statically (declared in ch_control.h) so tests can drive the
 * dispatcher directly without constructing a full IHS_SessionPacket. */

static void OnServerHandshake(IHS_SessionChannel *channel, const CServerHandshakeMsg *message);

static void OnSetClientConfig(IHS_SessionChannel *channel, const CSetStreamingClientConfig *message);

static void OnSetSpectatorMode(IHS_SessionChannel *channel, const CSetSpectatorModeMsg *message);

static void OnSetQoS(IHS_SessionChannel *channel, const CSetQoSMsg *message);

static const char *ControlMessageTypeName(EStreamControlMessage type);

static const IHS_SessionChannelClass ChannelClass = {
        .init = OnControlInit,
        .deinit = OnControlDeinit,
        .received = OnControlReceived,
        .instanceSize = sizeof(IHS_SessionChannelControl)
};

IHS_SessionChannel *IHS_SessionChannelControlCreate(IHS_Session *session) {
    return IHS_SessionChannelCreate(&ChannelClass, session, IHS_SessionChannelTypeControl, IHS_SessionChannelIdControl,
                                    NULL);
}

/* Bounded diagnostic snapshots. Wire length and truncation are explicit;
 * this is not a complete packet capture. Initialized before session workers. */
#define HID_REPORT_RING_LEN 128
#define HID_REPORT_RING_CAP 96
#define HID_PENDING_CAP 512
#define HID_PENDING_LEN 2048
typedef struct HIDDiagnostic {
    uint64_t ms;
    size_t wireLen;
    uint16_t len;
    uint8_t data[HID_PENDING_CAP];
} HIDDiagnostic;
static HIDDiagnostic hidReportRing[HID_REPORT_RING_LEN], hidPending[HID_PENDING_LEN];
static uint32_t hidReportRingHead, hidReportRingCount;
static uint32_t hidPendingHead, hidPendingTail, hidPendingCount;
static uint64_t hidPendingDropped;
static IHS_Mutex *hidPendingLock;

void IHS_ControlDiagnosticsInit(void) {
    hidPendingLock = IHS_MutexCreate();
    hidReportRingHead = hidReportRingCount = 0;
    hidPendingHead = hidPendingTail = hidPendingCount = 0;
    hidPendingDropped = 0;
}

void IHS_ControlDiagnosticsQuit(void) {
    IHS_MutexDestroy(hidPendingLock);
    hidPendingLock = NULL;
}

static void HIDPendingPush(uint64_t ms, const uint8_t *data, size_t len) {
    HIDDiagnostic record = {.ms = ms, .wireLen = len,
        .len = len < HID_PENDING_CAP ? len : HID_PENDING_CAP};
    memcpy(record.data, data, record.len);
    IHS_MutexLock(hidPendingLock);
    hidReportRing[hidReportRingHead] = record;
    hidReportRingHead = (hidReportRingHead + 1) % HID_REPORT_RING_LEN;
    if (hidReportRingCount < HID_REPORT_RING_LEN) hidReportRingCount++;
    /* Keep recent evidence when disk falls behind, accounting for every drop. */
    if (hidPendingCount == HID_PENDING_LEN) {
        hidPendingTail = (hidPendingTail + 1) % HID_PENDING_LEN;
        hidPendingCount--;
        hidPendingDropped++;
    }
    hidPending[hidPendingHead] = record;
    hidPendingHead = (hidPendingHead + 1) % HID_PENDING_LEN;
    hidPendingCount++;
    IHS_MutexUnlock(hidPendingLock);
}

size_t IHS_SessionChannelControlDrainPendingHIDReports(char *out, size_t cap) {
    if (out == NULL || cap == 0) return 0;
    size_t written = 0;
    out[0] = '\0';
    for (;;) {
        char line[1280];
        IHS_MutexLock(hidPendingLock);
        if (hidPendingCount == 0) {
            IHS_MutexUnlock(hidPendingLock);
            break;
        }
        const HIDDiagnostic *record = &hidPending[hidPendingTail];
        int prefix = snprintf(line, sizeof(line),
            "hidrep ms=%llu len=%u wire_len=%zu dropped=%llu ",
            (unsigned long long) record->ms, record->len, record->wireLen,
            (unsigned long long) hidPendingDropped);
        size_t lineLen = prefix > 0 ? (size_t) prefix : sizeof(line);
        if (lineLen + 2u * record->len + 1 >= sizeof(line) ||
            lineLen + 2u * record->len + 1 >= cap - written) {
            IHS_MutexUnlock(hidPendingLock);
            break; /* Do not consume a record that does not fit, including NUL. */
        }
        static const char hex[] = "0123456789abcdef";
        for (uint16_t i = 0; i < record->len; i++) {
            line[lineLen++] = hex[record->data[i] >> 4];
            line[lineLen++] = hex[record->data[i] & 15];
        }
        line[lineLen++] = '\n';
        hidPendingTail = (hidPendingTail + 1) % HID_PENDING_LEN;
        hidPendingCount--;
        IHS_MutexUnlock(hidPendingLock);
        memcpy(out + written, line, lineLen);
        written += lineLen;
        out[written] = '\0';
    }
    return written;
}

void IHS_SessionChannelControlGetRecentHIDReports(uint64_t *out_ms, uint16_t *out_len,
                                                  uint8_t *out_data, size_t *out_off) {
    IHS_MutexLock(hidPendingLock);
    if (*out_off >= hidReportRingCount) {
        *out_off = SIZE_MAX;
    } else {
        uint32_t slot = (hidReportRingHead + HID_REPORT_RING_LEN - 1 - *out_off)
                       % HID_REPORT_RING_LEN;
        *out_ms = hidReportRing[slot].ms;
        *out_len = hidReportRing[slot].len < HID_REPORT_RING_CAP ?
            hidReportRing[slot].len : HID_REPORT_RING_CAP;
        memcpy(out_data, hidReportRing[slot].data, *out_len);
        (*out_off)++;
    }
    IHS_MutexUnlock(hidPendingLock);
}

static bool ControlSendLocked(IHS_SessionChannelControl *control, EStreamControlMessage type,
                              const ProtobufCMessage *message, int32_t packetId,
                              uint16_t *assignedPacketId);


bool IHS_SessionChannelControlSend(IHS_SessionChannel *channel, EStreamControlMessage type,
                                   const ProtobufCMessage *message, int32_t packetId) {
    assert(channel->id == IHS_SessionChannelIdControl);
    IHS_SessionChannelControl *control = (IHS_SessionChannelControl *) channel;
    IHS_MutexLock(control->sendLock);
    bool ret = ControlSendLocked(control, type, message, packetId, NULL);
    IHS_MutexUnlock(control->sendLock);
    if (!ret) {
        IHS_SessionDisconnect(channel->session);
    }
    return ret;
}

static bool ControlSendLocked(IHS_SessionChannelControl *control, EStreamControlMessage type,
                              const ProtobufCMessage *message, int32_t packetId,
                              uint16_t *assignedPacketId) {
    IHS_SessionChannel *channel = &control->base;
    assert(channel->id == IHS_SessionChannelIdControl);
    size_t messageCapacity = protobuf_c_message_get_packed_size(message);
    const ProtobufCEnumValue *value = protobuf_c_enum_descriptor_get_value(&estream_control_message__descriptor,
                                                                           type);
    enum IHS_LogLevel logLevel;
    switch (type) {
        case k_EStreamControlRemoteHID:
            logLevel = IHS_LogLevelVerbose;
            break;
        default:
            logLevel = IHS_LogLevelDebug;
            break;
    }
    IHS_SessionFrame frame;
    IHS_SessionChannelInitializeFrame(channel, &frame, IHS_SessionPacketTypeReliable, true, packetId);
    frame.header.hidReport = type == k_EStreamControlRemoteHID;
    if (assignedPacketId != NULL) {
        *assignedPacketId = frame.header.packetId;
    }
    IHS_SessionLog(channel->session, logLevel, "Control", "Send control message: %s, id=%u", value->name,
                   frame.header.packetId);
    IHS_BufferAppendUInt8(&frame.body, type);
    if (IsMessageEncrypted(type)) {
        size_t cipherSize = EncryptedMessageCapacity(messageCapacity);
        uint8_t *serialized = calloc(1, messageCapacity ? messageCapacity : 1);
        if (serialized == NULL) {
            IHS_SessionFrameClear(&frame, true);
            return false;
        }
        size_t serializedLen = protobuf_c_message_pack(message, serialized);
        uint8_t *cipher = IHS_BufferPointerForAppend(&frame.body, cipherSize);
        if (IHS_SessionFrameEncrypt(channel->session, serialized, serializedLen, cipher, &cipherSize,
                                    control->sendEncryptSequence++) != 0) {
            free(serialized);
            IHS_SessionFrameClear(&frame, true);
            IHS_SessionLog(channel->session, IHS_LogLevelError, "Control", "Failed to encrypt payload\n");
            return false;
        }
        free(serialized);
        frame.body.size += cipherSize;
    } else {
        IHS_BufferAppendMessage(&frame.body, message);
    }
    bool ret = IHS_SessionChannelQueueFrame(channel, &frame, true);
    IHS_SessionFrameClear(&frame, true);
    return ret;
}

/* Official input path (docs/STEAMLINK_PROTOCOL_RE.md §9b.2): each 8ms tick
 * sends every device's delta in ONE message; there is no input-layer in-flight
 * window, coalescing, or supersede — reliable delivery is the transport's job
 * (retransmission until ACK/NACK). Superseding an unacked DELTA would break
 * the delta chain on the host. */
bool IHS_SessionChannelControlSubmitHIDReport(IHS_SessionChannel *channel,
                                              const uint8_t *data, size_t dataLen,
                                              bool activeInput) {
    assert(channel->id == IHS_SessionChannelIdControl);
    IHS_SessionChannelControl *control = (IHS_SessionChannelControl *) channel;
    IHS_MutexLock(control->sendLock);
    control->hidSubmitted++;
    CRemoteHIDMsg wrapped = CREMOTE_HIDMSG__INIT;
    wrapped.has_data = true;
    wrapped.data.data = (uint8_t *) data;
    wrapped.data.len = dataLen;
    /* Official passes the tick's actual input-activity state here
     * (SendRemoteHIDMessage bool → CRemoteHIDMsg.active_input, 0x7ab4c8). */
    wrapped.has_active_input = true;
    wrapped.active_input = activeInput;
    bool ret = ControlSendLocked(control, k_EStreamControlRemoteHID,
                                 (const ProtobufCMessage *) &wrapped, IHS_PACKET_ID_NEXT,
                                 NULL);
    if (ret) control->hidSent++;
    if (ret) HIDPendingPush(IHS_TimerNow(), data, dataLen);
    IHS_MutexUnlock(control->sendLock);
    if (!ret) {
        IHS_SessionDisconnect(channel->session);
    }
    return ret;
}

bool IHS_SessionChannelControlFlushPendingHID(IHS_SessionChannel *channel) {
    assert(channel->id == IHS_SessionChannelIdControl);
    IHS_UNUSED(channel);
    /* Kept for API compatibility: reports are sent on submit, matching the
     * official client. The transport retransmits until they are delivered. */
    return true;
}


void IHS_SessionChannelControlHandshake(IHS_SessionChannel *channel, bool networkTest) {
    // Idempotent — a duplicate ConnectACK on the wire can re-trigger this; skip if we're
    // already past the handshake phase.
    if (channel->session->state.connectionState >= IHS_SessionConnectionStateHandshaking) {
        return;
    }
    channel->session->state.connectionState = IHS_SessionConnectionStateHandshaking;
    CClientHandshakeMsg message = CCLIENT_HANDSHAKE_MSG__INIT;
    CStreamingClientHandshakeInfo handshakeInfo = CSTREAMING_CLIENT_HANDSHAKE_INFO__INIT;
    if (networkTest) {
        PROTOBUF_C_SET_VALUE(handshakeInfo, network_test, true);
    }
    message.info = &handshakeInfo;
    IHS_SessionChannelControlSend(channel, k_EStreamControlClientHandshake, (const ProtobufCMessage *) &message,
                                  IHS_PACKET_ID_NEXT);
}

static uint64_t ControlFeedbackTick(int count, void *context) {
    IHS_UNUSED(count);
    IHS_SessionChannelControlUpdateFeedback(context);
    return 5;
}

static void ControlFeedbackEnd(void *context) {
    ((IHS_SessionChannelControl *) context)->feedbackTimer = NULL;
}

void IHS_SessionChannelControlUpdateFeedback(IHS_SessionChannelControl *control) {
    IHS_MutexLock(control->receiveLock);
    ControlSendGapNack(control);
    IHS_MutexUnlock(control->receiveLock);
}

static void OnControlInit(IHS_SessionChannel *channel, const void *data) {
    IHS_UNUSED(data);
    IHS_SessionChannelControl *control = (IHS_SessionChannelControl *) channel;
    control->sendLock = IHS_MutexCreate();
    control->receiveLock = IHS_MutexCreate();
    control->framePacketWindow = IHS_SessionPacketsWindowCreateReliable(320, 0);
    control->feedbackTimer = IHS_TimerTaskStart(channel->session->timers,
        ControlFeedbackTick, ControlFeedbackEnd, 5, control);
}

static void OnControlDeinit(IHS_SessionChannel *channel) {
    IHS_SessionChannelControl *control = (IHS_SessionChannelControl *) channel;
    IHS_SessionChannelControlStopHeartbeat(channel);
    if (control->feedbackTimer) IHS_TimerTaskStopImmediate(control->feedbackTimer);
    IHS_SessionPacketsWindowDestroy(control->framePacketWindow);
    IHS_MutexDestroy(control->receiveLock);
    IHS_MutexDestroy(control->sendLock);
}

static void OnControlReceived(IHS_SessionChannel *channel, IHS_SessionPacket *packet) {
    IHS_SessionChannelControl *control = (IHS_SessionChannelControl *) channel;
    IHS_SessionPacketsWindow *window = control->framePacketWindow;
    /* ACK the contiguous packet-receipt watermark, including fragments of an
     * incomplete frame. This callback treats each received datagram as a batch. */
    switch (packet->header.type) {
        case IHS_SessionPacketTypeReliable:
        case IHS_SessionPacketTypeReliableFrag:
            IHS_MutexLock(control->receiveLock);
            control->peerTimestamp = packet->header.sendTimestamp;
            control->peerReceiveTime = IHS_SessionPacketTimestamp();
            control->receivedReliable = true;
            bool added = IHS_SessionPacketsWindowAdd(window, packet);
            IHS_MutexUnlock(control->receiveLock);
            if (!added) {
                /* Log and disconnect once: the packets keep coming, and firing a
                 * disconnect per packet floods the link and re-enters teardown. */
                if (!control->overflowed) {
                    control->overflowed = true;
                    IHS_SessionLog(channel->session, IHS_LogLevelError, "Control", "Frames window overflow");
                    IHS_SessionDisconnect(channel->session);
                }
                return;
            }
            break;
        case IHS_SessionPacketTypeACK:
            /* SessionRecvCallback already processed the cumulative ACK. */
            break;
        case IHS_SessionPacketTypeNACK:
            ControlOnNackPacket(control, packet);
            break;
        default:
            /* Official ignores unrecognized channel packet types (its channel
             * HandlePacket default falls through without action). Killing the
             * session on one unexpected packet turns a harmless difference
             * into a dead stream. */
            IHS_SessionLog(channel->session, IHS_LogLevelWarn, "Control",
                           "Ignoring unrecognized packet type %u\n", packet->header.type);
            break;
    }
    IHS_SessionFrame frame;
    IHS_BufferInit(&frame.body, 1024, 1024 * 1024);

    for (;;) {
        IHS_MutexLock(control->receiveLock);
        bool ready = IHS_SessionPacketsWindowPoll(window, &frame);
        IHS_MutexUnlock(control->receiveLock);
        if (!ready) break;
        if (frame.body.size == 0) {
            IHS_SessionPacketsWindowReleaseFrame(&frame);
            continue;
        }
        EStreamControlMessage type = *IHS_BufferPointer(&frame.body);
        IHS_BufferOffsetBy(&frame.body, 1);
        if (IsMessageEncrypted(type)) {
            IHS_Buffer plain;
            IHS_BufferInit(&plain, 1024, 1024 * 1024);
            /* OnStreamPacket 0x7ad054..0x7ad070 increments BEFORE BDecrypt,
             * including failed attempts. No resynchronization or rollback. */
            uint64_t expectSequence = control->recvEncryptSequence++, actualSequence;
            switch (IHS_SessionFrameDecrypt(channel->session, &frame.body, &plain, expectSequence, &actualSequence)) {
                case IHS_SessionPacketResultOK: {
                    IHS_SessionChannelControlOnMessageReceived(channel, type, &plain, &frame.header);
                    break;
                }
                case IHS_SessionFrameDecryptHashMismatch:
                case IHS_SessionFrameDecryptOldSequence: {
                    // Ignore this packet
                    break;
                }
                case IHS_SessionFrameDecryptSequenceMismatch: {
                    IHS_SessionLog(channel->session, IHS_LogLevelWarn, "Control",
                                   "Mismatched message sequence %llu (expect %llu). id=%d, retransmit=%d, type=%s",
                                   actualSequence, expectSequence, frame.header.packetId, frame.header.retransmitCount,
                                   ControlMessageTypeName(type));
                    break;
                }
                case IHS_SessionFrameDecryptFailed: {
                    IHS_SessionLog(channel->session, IHS_LogLevelWarn, "Control",
                                   "Failed to decrypt control message. id=%d, retransmit=%d, type=%s",
                                   frame.header.packetId, frame.header.retransmitCount, ControlMessageTypeName(type));
                    break;
                }
            }
            IHS_BufferClear(&plain, true);
        } else {
            IHS_SessionChannelControlOnMessageReceived(channel, type, &frame.body, &frame.header);
        }
        IHS_SessionPacketsWindowReleaseFrame(&frame);
    }

    IHS_BufferClear(&frame.body, true);

    /* Confirm received packets even if their complete frame is still pending.
     * A duplicate must re-ACK, otherwise loss of the last ACK never heals. */
    IHS_MutexLock(control->receiveLock);
    if (packet->header.type == IHS_SessionPacketTypeReliable ||
        packet->header.type == IHS_SessionPacketTypeReliableFrag) {
        uint16_t confirmed = IHS_SessionPacketsWindowContiguousId(window);
        IHS_SessionChannelPacketAck(channel, confirmed, 0, true,
            control->peerTimestamp + IHS_SessionPacketTimestamp() - control->peerReceiveTime);
    }
    ControlSendGapNack(control);
    IHS_MutexUnlock(control->receiveLock);
}

/* Peer reports its receive state for OUR reliable stream. Extended NACK body
 * (wire offsets after the 13B header): [0..3] u32 timestamp, [4..5] u16
 * contiguous delivery point, [6..] presence mask (bit=1: peer holds it).
 * bit=1 releases the packet from retransmission; bit=0 forces a resend.
 * Header-only (empty body) is the simple form: everything the reference
 * (header.packetId) covers is missing on the peer. */
static void ControlOnNackPacket(IHS_SessionChannelControl *control, IHS_SessionPacket *packet) {
    IHS_Session *session = control->base.session;
    uint64_t nowMs = IHS_TimerNow();
    /* HandleNackPacket 0x7f9610: ignore stale feedback, allowing equality. */
    if (control->haveNackTimestamp &&
        (int32_t) (packet->header.sendTimestamp - control->lastNackTimestamp) < 0) return;
    if (packet->body.size != 0 && packet->body.size < 6) return;
    control->haveNackTimestamp = true;
    control->lastNackTimestamp = packet->header.sendTimestamp;
    uint32_t cutoff = IHS_SessionPacketTimestamp() - IHS_StreamClockNackAgeTicks(&session->clock);
    if (packet->body.size >= 6) {
        const uint8_t *body = IHS_BufferPointer(&packet->body);
        uint32_t seen = body[0] | (uint32_t)body[1]<<8 | (uint32_t)body[2]<<16 | (uint32_t)body[3]<<24;
        /* csel ..., lt at 0x7f977c selects seen when cutoff < seen (max). */
        if ((int32_t)(cutoff - seen) < 0) cutoff = seen;
        uint16_t contiguous = (uint16_t) (body[4] | (body[5] << 8));
        IHS_RetransmissionAcknowledgeThrough(&session->retransmission,
            IHS_SessionChannelIdControl, (uint16_t) (contiguous + 1u), nowMs);
        size_t maskLen = packet->body.size - 6;
        for (size_t j = 0; j < maskLen; j++) {
            uint8_t bits = body[6 + j];
            /* 0x7f9840 skips zero bytes; 0x7f9868..78 stops after the highest
             * set bit. Trailing zero bits are not explicit resend requests. */
            for (size_t k = 0; bits; k++, bits >>= 1) {
                uint16_t id = (uint16_t) (packet->header.packetId + j * 8 + k);
                if (bits & 1u) {
                    IHS_RetransmissionAcknowledge(&session->retransmission,
                        IHS_SessionChannelIdControl, id, INT16_MIN, nowMs);
                } else {
                    IHS_RetransmissionNackBefore(&session->retransmission,
                        IHS_SessionChannelIdControl, id, false, cutoff, nowMs);
                }
            }
        }
    }
    /* Header-only and extended forms also request IDs BELOW the bitmap base. */
    IHS_RetransmissionNackBefore(&session->retransmission,
        IHS_SessionChannelIdControl, packet->header.packetId, true, cutoff, nowMs);
}

static void ControlSendGapNack(IHS_SessionChannelControl *control) {
    IHS_Session *session = control->base.session;
    IHS_SessionPacketsWindow *window = control->framePacketWindow;
    /* Only when a packet slot is actually MISSING: a partially received
     * frame (fragments still in flight) leaves all buffered slots used and
     * must not trigger a NACK. Official scans its receive ring the same way
     * (UpdateReliableState 0x7f9d0c). */
    if (!IHS_SessionPacketsWindowHasHole(window)) {
        return;
    }
    uint16_t confirmed = IHS_SessionPacketsWindowContiguousId(window);
    uint16_t needed = (uint16_t) (confirmed + 1u);
    uint64_t nowMs = IHS_TimerNow();
    /* Official hole-age threshold is 65 units (~1 ms, [f47000+2444] from the
     * 0x7fe284 initializer); the effective cadence is the channel update tick
     * (5 ms), which the retransmission tick matches. */
    if (control->lastNackSentMs != 0 &&
        nowMs - control->lastNackSentMs < 5) {
        return;
    }
    control->lastNackSentMs = nowMs;

    /* Extended NACK: presence mask covering up to 320 IDs from the base. */
    uint8_t bitmap[40];
    IHS_SessionPacketsWindowHoleBitmap(window, needed, bitmap, 320);

    IHS_SessionPacket packet;
    IHS_SessionChannelInitializePacket(&control->base, &packet, IHS_SessionPacketTypeNACK,
                                       false, needed);
    packet.header.fragmentId = 0;
    IHS_BufferAppendUInt32LE(&packet.body, control->peerTimestamp);
    IHS_BufferAppendUInt16LE(&packet.body, confirmed);
    IHS_BufferAppendMem(&packet.body, bitmap, sizeof(bitmap));
    IHS_SessionChannelQueuePacket(&control->base, &packet, false);
    IHS_SessionPacketClear(&packet, true);
}

void IHS_SessionChannelControlOnMessageReceived(IHS_SessionChannel *channel, EStreamControlMessage type,
                                                IHS_Buffer *payload, const IHS_SessionPacketHeader *header) {
    switch (type) {
        case k_EStreamControlServerHandshake: {
            CServerHandshakeMsg *message = IHS_UNPACK_BUFFER(cserver_handshake_msg__unpack, payload);
            if (message == NULL) {
                IHS_SessionLog(channel->session, IHS_LogLevelWarn, "Control", "Malformed CServerHandshakeMsg");
                break;
            }
            OnServerHandshake(channel, message);
            cserver_handshake_msg__free_unpacked(message, NULL);
            break;
        }
        case k_EStreamControlAuthenticationResponse: {
            IHS_SessionChannelControlOnAuthentication(channel, type, payload, header);
            break;
        }
        case k_EStreamControlNegotiationInit:
        case k_EStreamControlNegotiationSetConfig: {
            IHS_SessionChannelControlOnNegotiation(channel, type, payload, header);
            break;
        }
        case k_EStreamControlSetStreamingClientConfig: {
            CSetStreamingClientConfig *message = IHS_UNPACK_BUFFER(cset_streaming_client_config__unpack, payload);
            if (message == NULL) {
                IHS_SessionLog(channel->session, IHS_LogLevelWarn, "Control", "Malformed CSetStreamingClientConfig");
                break;
            }
            OnSetClientConfig(channel, message);
            cset_streaming_client_config__free_unpacked(message, NULL);
            break;
        }
        case k_EStreamControlSetSpectatorMode: {
            CSetSpectatorModeMsg *message = IHS_UNPACK_BUFFER(cset_spectator_mode_msg__unpack, payload);
            if (message == NULL) {
                IHS_SessionLog(channel->session, IHS_LogLevelWarn, "Control", "Malformed CSetSpectatorModeMsg");
                break;
            }
            OnSetSpectatorMode(channel, message);
            cset_spectator_mode_msg__free_unpacked(message, NULL);
            break;
        }
        case k_EStreamControlStartAudioData:
        case k_EStreamControlStopAudioData: {
            IHS_SessionChannelControlOnAudio(channel, type, payload, header);
            break;
        }
        case k_EStreamControlStartMicrophoneData:
        case k_EStreamControlStopMicrophoneData: {
            IHS_SessionChannelControlOnMicrophone(channel, type, payload, header);
            break;
        }
        case k_EStreamControlStartVideoData:
        case k_EStreamControlStopVideoData:
        case k_EStreamControlVideoEncoderInfo:
        case k_EStreamControlSetCaptureSize:
        case k_EStreamControlSetTargetFramerate:
        case k_EStreamControlSetTargetBitrate:
        case k_EStreamControlSetQualityOverride:
        case k_EStreamControlSetBitrateOverride:
        case k_EStreamControlEnableHighResCapture:
        case k_EStreamControlDisableHighResCapture: {
            IHS_SessionChannelControlOnVideo(channel, type, payload, header);
            break;
        }
        case k_EStreamControlSetQoS: {
            CSetQoSMsg *message = IHS_UNPACK_BUFFER(cset_qo_smsg__unpack, payload);
            if (message == NULL) {
                IHS_SessionLog(channel->session, IHS_LogLevelWarn, "Control", "Malformed CSetQoSMsg");
                break;
            }
            OnSetQoS(channel, message);
            cset_qo_smsg__free_unpacked(message, NULL);
            break;
        }
        case k_EStreamControlShowCursor:
        case k_EStreamControlHideCursor:
        case k_EStreamControlSetCursor:
        case k_EStreamControlGetCursorImage:
        case k_EStreamControlSetCursorImage:
        case k_EStreamControlDeleteCursor: {
            IHS_SessionChannelControlOnCursor(channel, type, payload, header);
            break;
        }
        case k_EStreamControlSetKeymap: {
            CSetKeymapMsg *message = IHS_UNPACK_BUFFER(cset_keymap_msg__unpack, payload);
            if (message == NULL) {
                IHS_SessionLog(channel->session, IHS_LogLevelWarn, "Control", "Malformed CSetKeymapMsg");
                break;
            }
            const IHS_StreamInputCallbacks *callbacks = channel->session->callbacks.input;
            void *context = channel->session->callbackContexts.input;
            /* Steam treats SetKeymap as an overlay-relabel hint, not a translation
             * table for keystrokes the client sends back. Convert each protobuf
             * row into a stable C-struct so the caller doesn't need to depend on
             * the generated protobuf headers, then hand it to the callback. */
            if (callbacks && callbacks->setKeymap && message->keymap != NULL) {
                size_t n = message->keymap->n_entries;
                IHS_KeymapEntry *entries = n > 0 ? calloc(n, sizeof(IHS_KeymapEntry)) : NULL;
                if (n == 0 || entries != NULL) {
                    for (size_t i = 0; i < n; i++) {
                        CStreamingKeymapEntry *src = message->keymap->entries[i];
                        IHS_KeymapEntry *dst = &entries[i];
                        dst->scancode = src->has_scancode ? src->scancode : 0;
                        dst->normal_keycode = src->has_normal_keycode ? src->normal_keycode : 0;
                        dst->shift_keycode = src->has_shift_keycode ? src->shift_keycode : 0;
                        dst->capslock_keycode = src->has_capslock_keycode ? src->capslock_keycode : 0;
                        dst->shift_capslock_keycode = src->has_shift_capslock_keycode
                                                    ? src->shift_capslock_keycode : 0;
                        dst->altgr_keycode = src->has_altgr_keycode ? src->altgr_keycode : 0;
                        dst->altgr_shift_keycode = src->has_altgr_shift_keycode ? src->altgr_shift_keycode : 0;
                        dst->altgr_capslock_keycode = src->has_altgr_capslock_keycode
                                                    ? src->altgr_capslock_keycode : 0;
                        dst->altgr_shift_capslock_keycode = src->has_altgr_shift_capslock_keycode
                                                          ? src->altgr_shift_capslock_keycode : 0;
                    }
                    callbacks->setKeymap(channel->session, entries, n, context);
                }
                free(entries);
            }
            cset_keymap_msg__free_unpacked(message, NULL);
            break;
        }
        case k_EStreamControlSetCapslock: {
            CSetCapslockMsg *message = IHS_UNPACK_BUFFER(cset_capslock_msg__unpack, payload);
            if (message == NULL) {
                IHS_SessionLog(channel->session, IHS_LogLevelWarn, "Control", "Malformed CSetCapslockMsg");
                break;
            }
            bool pressed = message->has_pressed ? message->pressed : false;
            IHS_SessionLog(channel->session, IHS_LogLevelDebug, "Control", "SetCapsLock(%s)",
                           pressed ? "on" : "off");
            const IHS_StreamInputCallbacks *callbacks = channel->session->callbacks.input;
            if (callbacks && callbacks->setCapsLock) {
                callbacks->setCapsLock(channel->session, pressed, channel->session->callbackContexts.input);
            }
            cset_capslock_msg__free_unpacked(message, NULL);
            break;
        }
        case k_EStreamControlSetTitle: {
            CSetTitleMsg *message = IHS_UNPACK_BUFFER(cset_title_msg__unpack, payload);
            if (message == NULL) {
                IHS_SessionLog(channel->session, IHS_LogLevelWarn, "Control", "Malformed CSetTitleMsg");
                break;
            }
            IHS_SessionLog(channel->session, IHS_LogLevelInfo, "Control", "Set title: %s", message->text);
            cset_title_msg__free_unpacked(message, NULL);
            break;
        }
        case k_EStreamControlSetIcon:
        case k_EStreamControlSetActivity:
            break;
        case k_EStreamControlRemoteHID: {
            CRemoteHIDMsg *message = IHS_UNPACK_BUFFER(cremote_hidmsg__unpack, payload);
            if (message == NULL) {
                IHS_SessionLog(channel->session, IHS_LogLevelWarn, "Control", "Malformed CRemoteHIDMsg");
                break;
            }
            if (message->has_data) {
                CHIDMessageToRemote *hid = chidmessage_to_remote__unpack(NULL, message->data.len, message->data.data);
                if (hid != NULL) {
                    IHS_SessionChannelControlOnHIDMsg(channel, hid);
                    chidmessage_to_remote__free_unpacked(hid, NULL);
                } else {
                    IHS_SessionLog(channel->session, IHS_LogLevelWarn, "Control",
                                   "Malformed CHIDMessageToRemote inside CRemoteHIDMsg");
                }
            }
            cremote_hidmsg__free_unpacked(message, NULL);
            break;
        }
        case k_EStreamControlControllerConfigMsg: {
            CControllerConfigMsg *message = IHS_UNPACK_BUFFER(ccontroller_config_msg__unpack, payload);
            if (message == NULL) {
                IHS_SessionLog(channel->session, IHS_LogLevelWarn, "Control", "Malformed CControllerConfigMsg");
                break;
            }
            ccontroller_config_msg__free_unpacked(message, NULL);
            break;
        }
        case k_EStreamControlControllerPersonalizationUpdate: {
            CControllerPersonalizationUpdateMsg *message = IHS_UNPACK_BUFFER(
                    ccontroller_personalization_update_msg__unpack, payload);
            if (message == NULL) {
                IHS_SessionLog(channel->session, IHS_LogLevelWarn, "Control",
                               "Malformed CControllerPersonalizationUpdateMsg");
                break;
            }
            ccontroller_personalization_update_msg__free_unpacked(message, NULL);
            break;
        }
        case k_EStreamControlSetInputTemporarilyDisabled: {
            CSetInputTemporarilyDisabledMsg *message = IHS_UNPACK_BUFFER(
                cset_input_temporarily_disabled_msg__unpack, payload);
            if (message != NULL) {
                bool was = channel->session->state.inputTemporarilyDisabled;
                channel->session->state.inputTemporarilyDisabled = message->disabled;
                IHS_SessionLog(channel->session, IHS_LogLevelInfo, "Control",
                               "Input temporarily disabled: %u -> %u", was,
                               message->disabled);
                cset_input_temporarily_disabled_msg__free_unpacked(message, NULL);
            }
            break;
        }
        case k_EStreamControlStopRequest:
            /* Host quit the game: official clients end the session on this. */
            IHS_SessionHostStopped(channel->session);
            break;
        case k_EStreamControlKeepAlive:
            /* Official consumes KeepAlive inline before dispatch
             * (OnStreamPacket 0x7ad0b4) — no log, no response. */
            break;
        case k_EStreamControlCaptureFailed:
            IHS_SessionLog(channel->session, IHS_LogLevelError, "Control",
                           "Host capture failed (OnCaptureFailed)");
            break;
        case k_EStreamControlSystemSuspend:
            IHS_SessionLog(channel->session, IHS_LogLevelWarn, "Control",
                           "Host is suspending (OnSystemSuspend)");
            break;
        default: {
            IHS_SessionLog(channel->session, IHS_LogLevelInfo, "Control", "Unhandled control message: %s",
                           ControlMessageTypeName(type));
            break;
        }
    }
}


static void OnServerHandshake(IHS_SessionChannel *channel, const CServerHandshakeMsg *message) {
    if (message->info == NULL) {
        IHS_SessionLog(channel->session, IHS_LogLevelWarn, "Control", "ServerHandshake missing info");
        return;
    }
    IHS_Session *session = channel->session;
    // Drop stale ServerHandshakes that arrive after we've moved on to auth/negotiation.
    if (session->state.connectionState != IHS_SessionConnectionStateHandshaking) {
        return;
    }
    if (message->info->has_mtu) {
        session->state.mtu = message->info->mtu;
    } else {
        session->state.mtu = 1500;
    }
    IHS_SessionChannelControlRequestAuthentication(channel);
}

static void OnSetClientConfig(IHS_SessionChannel *channel, const CSetStreamingClientConfig *message) {
    const CStreamingClientConfig *config = message->config;
    if (config == NULL) {
        IHS_SessionLog(channel->session, IHS_LogLevelWarn, "Control", "SetStreamingClientConfig missing config");
        return;
    }
    IHS_Session *session = channel->session;
    // Mirror Steam's BStreamingInput/Audio/Video reading from the negotiated client config.
    // Each flag is only updated if the server actually included it; absent fields keep the
    // current value (which started at true at session creation).
    if (config->has_enable_input_streaming) {
        session->state.streamingInput = config->enable_input_streaming;
    }
    if (config->has_enable_audio_streaming) {
        session->state.streamingAudio = config->enable_audio_streaming;
    }
    if (config->has_enable_video_streaming) {
        session->state.streamingVideo = config->enable_video_streaming;
    }
    IHS_SessionLog(session, IHS_LogLevelDebug, "Control",
                   "Set client config. input=%u audio=%u video=%u enable_video_hevc=%u",
                   session->state.streamingInput, session->state.streamingAudio,
                   session->state.streamingVideo, config->enable_video_hevc);
}

bool IHS_SessionInputEnabled(IHS_Session *session) {
    /* Host-issued temporary disable (scene transitions etc.) suppresses ALL
     * input reports, matching the official client's OnSetInputTemporarily
     * Disabled handling. The delta chain stays intact: while disabled no
     * deltas are submitted and `previous` does not advance, so the first
     * delta after re-enable carries the full accumulated change. */
    /* OnSetInputTemporarilyDisabled opens an informational dialog. Its
     * controller handlers return false (0x766cb8/0x766cc0), so it does not
     * suppress HID reports. Only negotiated enable_input_streaming gates input. */
    return session->state.streamingInput;
}

bool IHS_SessionStreaming(IHS_Session *session) {
    return session->state.connectionState == IHS_SessionConnectionStateConnected;
}

static void OnSetSpectatorMode(IHS_SessionChannel *channel, const CSetSpectatorModeMsg *message) {
    IHS_SessionLog(channel->session, IHS_LogLevelDebug, "Control", "Set client config. spectator_mode=%u",
                   message->enabled);
}

static void OnSetQoS(IHS_SessionChannel *channel, const CSetQoSMsg *message) {
    IHS_SessionLog(channel->session, IHS_LogLevelDebug, "Control", "Set QoS config. use_qos=%u",
                   message->use_qos);
}

static bool IsMessageEncrypted(EStreamControlMessage type) {
    switch (type) {
        case k_EStreamControlClientHandshake:
        case k_EStreamControlServerHandshake:
        case k_EStreamControlAuthenticationRequest:
        case k_EStreamControlAuthenticationResponse:
            return false;
        default:
            return true;
    }
}

static size_t EncryptedMessageCapacity(size_t plainSize) {
    /* iv + pkcs7pad(sequence + plain) */
    return 16 + ((plainSize + sizeof(uint64_t)) / IHS_CRYPTO_AES_BLOCK_SIZE + 1) * IHS_CRYPTO_AES_BLOCK_SIZE;
}

static const char *ControlMessageTypeName(EStreamControlMessage type) {
    const ProtobufCEnumValue *value = protobuf_c_enum_descriptor_get_value(&estream_control_message__descriptor,
                                                                           type);
    return value ? value->name : "unknown";
}
