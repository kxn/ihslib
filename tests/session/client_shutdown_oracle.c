/* Production CRC ingress, reliable -> control crypto/FIFO, update and stopping.
 * OS IO, stage6 body, decoder/input terminal bodies and next ID are controlled. */
#include "hid/manager.h"
#include "ihs_buffer.h"
#include "ihs_buffer_ext.h"
#include "ihslib.h"
#include "session/channels/ch_control.h"
#include "session/channels/ch_data.h"
#include "session/session_pri.h"
#include "test_session.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static IHS_Session *session;
static uint32_t now = 1000000;
static bool input, readable, trackPaused;
static IHS_Buffer datagram;
static char *events[16], *wires[32], *configs[16];
static unsigned configCount;
static const char *actions[32];
static unsigned eventCount, wireCount, actionCount;
static void action(const char *s) {
    assert(actionCount < 32);
    actions[actionCount++] = s;
}
static const char *media_name(IHS_SessionChannel *ch, bool destroy) {
    return ch->type == IHS_SessionChannelTypeDataAudio ? (destroy ? "audio_destroy" : "audio_stop")
                                                       : (destroy ? "video_destroy" : "video_stop");
}
static void media_init(IHS_SessionChannel *ch, const void *unused) {
    (void)unused;
    IHS_SessionChannelData *d = (void *)ch;
    d->window = IHS_SessionPacketsWindowCreateMedia(32);
    d->fec = IHS_FECReceiverCreate();
    d->windowLock = IHS_MutexCreate();
    d->windowCond = IHS_CondCreate();
    d->fecQueueCapacity = 32;
    d->fecQueueLimit = 2 * 1024 * 1024;
    atomic_init(&d->interrupted, false);
    assert(d->window && d->fec && d->windowLock && d->windowCond);
}
static void media_stop(IHS_SessionChannel *ch) { action(media_name(ch, false)); }
static void media_destroy(IHS_SessionChannel *ch) {
    action(media_name(ch, true));
    IHS_SessionChannelDataDeinit(ch);
}
static const IHS_SessionChannelClass mediaClass = {.instanceSize = sizeof(IHS_SessionChannelData),
                                                   .init = media_init,
                                                   .stopped = media_stop,
                                                   .deinit = media_destroy,
                                                   .received = IHS_SessionChannelDataReceived};
uint32_t __wrap_IHS_SessionPacketTimestamp(void) { return now; }
uint8_t __wrap_IHS_SessionNextConnectionId(void) { return 12; }
bool __wrap_IHS_BaseSend(IHS_Base *b, IHS_SocketAddress a, const IHS_Buffer *wire) {
    (void)b;
    (void)a;
    assert(wireCount < 32);
    char *s = malloc(wire->size * 2 + 1);
    assert(s);
    s[0] = 0;
    for (size_t i = 0; i < wire->size; ++i)
        sprintf(s + i * 2, "%02x", IHS_BufferPointer(wire)[i]);
    wires[wireCount++] = s;
    return true;
}
int __wrap_IHS_UDPSocketWaitReadable(IHS_UDPSocket *s, uint32_t ms) {
    (void)s;
    (void)ms;
    return readable;
}
int __wrap_IHS_UDPSocketReceive(IHS_UDPSocket *s, IHS_UDPPacket *p) {
    (void)s;
    if (!datagram.size)
        return 0;
    p->address = session->info.address;
    IHS_BufferAppend(&p->buffer, &datagram);
    IHS_BufferClear(&datagram, false);
    return 1;
}
void __real_IHS_SessionClientTransportClosed(IHS_Session *);
void __wrap_IHS_SessionClientTransportClosed(IHS_Session *s) {
    action("transport_closed");
    __real_IHS_SessionClientTransportClosed(s);
}
void __real_IHS_HIDManagerCloseAll(IHS_HIDManager *);
void __wrap_IHS_HIDManagerCloseAll(IHS_HIDManager *m) {
    if (input) {
        action("input_destroy");
        input = false;
    }
    __real_IHS_HIDManagerCloseAll(m);
}
void __real_IHS_SessionChannelControlUpdateStage(IHS_SessionChannel *, uint32_t);
void __wrap_IHS_SessionChannelControlUpdateStage(IHS_SessionChannel *ch, uint32_t tick) {
    int phase = session->state.connectionState;
    if (phase == IHS_SessionConnectionStateConnected) {
        action("streaming");
        return;
    }
    if (phase == IHS_SessionConnectionStateStarting) {
        action("starting");
        return;
    }
    if (phase == IHS_SessionConnectionStateStopping)
        action("stopping");
    __real_IHS_SessionChannelControlUpdateStage(ch, tick);
}
static void hex(FILE *f, const uint8_t *data, size_t size) {
    for (size_t i = 0; i < size; ++i)
        fprintf(f, "%02x", data[i]);
}
static void value(FILE *f, EStreamControlMessage type, const IHS_Buffer *body,
                  const IHS_SessionPacketHeader *h, const char *phase) {
    fprintf(f, "{\"type\":%u,\"body\":\"", type);
    hex(f, IHS_BufferPointer(body), body->size);
    fprintf(f, "\",\"sent\":%u,\"receive\":%u", h->sendTimestamp, h->receiveTimestamp);
    if (phase)
        fprintf(f, ",\"phase\":\"%s\"", phase);
    fputc('}', f);
}
void __real_IHS_SessionChannelControlOnMessageReceived(IHS_SessionChannel *, EStreamControlMessage,
                                                       IHS_Buffer *,
                                                       const IHS_SessionPacketHeader *);
void __wrap_IHS_SessionChannelControlOnMessageReceived(IHS_SessionChannel *ch,
                                                       EStreamControlMessage type, IHS_Buffer *body,
                                                       const IHS_SessionPacketHeader *header) {
    assert(type == 99 && eventCount < 16);
    action("control_99");
    size_t size = 0;
    FILE *f = open_memstream(&events[eventCount++], &size);
    assert(f);
    value(f, type, body, header, "drain");
    assert(!fclose(f));
    __real_IHS_SessionChannelControlOnMessageReceived(ch, type, body, header);
}
static void stopped(IHS_Session *s, void *ctx) {
    (void)ctx;
    if (s->state.connectionState == IHS_SessionConnectionStateStopping)
        action("player_stopped");
}
static void config_event(const char *kind, const uint8_t *wire, size_t size, bool local) {
    assert(configCount < 16);
    size_t bytes = 0;
    FILE *f = open_memstream(&configs[configCount++], &bytes);
    assert(f);
    fprintf(f, "{\"kind\":\"%s\",\"wire\":\"", kind);
    hex(f, wire, size);
    fputc('"', f);
    if (!strcmp(kind, "notify"))
        fprintf(f, ",\"local_only\":%s", local ? "true" : "false");
    fputc('}', f);
    assert(!fclose(f));
}
static void config_notify(IHS_Session *s, const uint8_t *wire, size_t size, bool local, void *ctx) {
    (void)s;
    (void)ctx;
    action("config_notify");
    config_event("notify", wire, size, local);
}
static void config_save(IHS_Session *s, const uint8_t *wire, size_t size, void *ctx) {
    (void)s;
    (void)ctx;
    action("config_save");
    config_event("save", wire, size, true);
}
static void protobuf(const ProtobufCMessage *m) {
    if (!m)
        return;
    size_t size = protobuf_c_message_get_packed_size(m);
    uint8_t *data = malloc(size ? size : 1);
    assert(data);
    protobuf_c_message_pack(m, data);
    hex(stdout, data, size);
    free(data);
}
static void report(void) {
    IHS_SessionChannelControl *control = (void *)session->channels[1];
    printf("{\"client\":%d,\"transport\":%d,\"queue\":[", session->state.connectionState,
           IHS_SessionTransportState(session));
    unsigned n = 0;
    for (IHS_IncomingControl *p = control->incomingHead; p; p = p->next) {
        if (n++)
            putchar(',');
        value(stdout, p->type, &p->body, &p->header, NULL);
    }
    printf("],\"events\":[");
    for (unsigned i = 0; i < eventCount; ++i) {
        printf("%s%s", i ? "," : "", events[i]);
        free(events[i]);
    }
    eventCount = 0;
    printf("],\"config\":\"");
    protobuf((void *)session->clientConfig);
    printf("\",\"sequence\":%llu,\"actions\":[", (unsigned long long)control->recvEncryptSequence);
    for (unsigned i = 0; i < actionCount; ++i)
        printf("%s\"%s\"", i ? "," : "", actions[i]);
    actionCount = 0;
    IHS_SessionChannel *audio = IHS_SessionChannelForType(session, IHS_SessionChannelTypeDataAudio),
                       *video = IHS_SessionChannelForType(session, IHS_SessionChannelTypeDataVideo);
    printf("],\"audio\":%s,\"video\":%s,\"audio_id\":%u,\"video_id\":%u,\"local\":%u,\"peer\":%u,"
           "\"wires\":[",
           audio ? "true" : "false", video ? "true" : "false", audio ? audio->id : 0,
           video ? video->id : 0, (unsigned)session->state.connectionId,
           (unsigned)session->state.hostConnectionId);
    for (unsigned i = 0; i < wireCount; ++i) {
        printf("%s\"%s\"", i ? "," : "", wires[i]);
        free(wires[i]);
    }
    wireCount = 0;
    if (trackPaused)
        printf("],\"client_paused\":%s,\"connection_paused\":%s,\"config_events\":[",
               session->transport.clientPaused ? "true" : "false",
               session->transport.connectionPaused ? "true" : "false");
    else
        printf("],\"config_events\":[");
    for (unsigned i = 0; i < configCount; ++i) {
        printf("%s%s", i ? "," : "", configs[i]);
        free(configs[i]);
    }
    configCount = 0;
    puts("]}");
}
static void packet(unsigned type, unsigned id, unsigned sent, const char *raw) {
    IHS_SessionPacket p = {.header = {.type = type,
                                      .channelId = type == 5 ? 1 : 0,
                                      .packetId = id,
                                      .srcConnectionId = 34,
                                      .dstConnectionId = 12,
                                      .hasCrc = session->state.crcRequired,
                                      .sendTimestamp = sent}};
    IHS_SessionPacketBodyInitialize(&p.body, p.header.hasCrc);
    for (size_t i = 0; raw[i]; i += 2) {
        unsigned b;
        assert(raw[i + 1] && sscanf(raw + i, "%2x", &b) == 1);
        IHS_BufferAppendUInt8(&p.body, b);
    }
    IHS_SessionPacketPopulateBuffer(&p);
    IHS_BufferAppendMem(&datagram, p.body.data, IHS_SessionPacketSize(&p));
    IHS_SessionPacketClear(&p, true);
}
int main(int argc, char **argv) {
    assert(argc == 4 || argc == 5);
    IHS_Init();
    session = IHS_TestSessionCreate();
    IHS_TimerTaskStopImmediate(session->retransmission.timer);
    session->state.connectionState = atoi(argv[1]);
    unsigned mask = atoi(argv[2]);
    input = mask & 4;
    session->state.connectionId = 12;
    session->state.hostConnectionId = 34;
    session->state.crcRequired = atoi(argv[3]);
    trackPaused = argc == 5 && atoi(argv[4]);
    if (trackPaused) {
        session->transport.clientPaused = session->transport.connectionPaused = true;
        session->transport.appliedGeneration = 1;
        atomic_store(&session->transport.lifecycleGeneration, 1);
    }
    session->transport.active = true;
    session->transport.lastReceive = 1000000;
    session->clock.lastEcho = 1000000;
    session->clock.haveEcho = true;
    session->outgoing.rate[0] = session->outgoing.rate[1] = 0;
    IHS_OutgoingRegister(session, 3);
    static const IHS_StreamSessionCallbacks cb = {.disconnected = stopped,
                                                  .clientConfigChanged = config_notify,
                                                  .saveClientConfig = config_save};
    IHS_SessionSetSessionCallbacks(session, &cb, NULL);
    for (unsigned i = 0; i < 2; ++i)
        if (mask & (1 << i))
            IHS_SessionChannelAdd(session,
                                  IHS_SessionChannelCreate(&mediaClass, session,
                                                           i ? IHS_SessionChannelTypeDataVideo
                                                             : IHS_SessionChannelTypeDataAudio,
                                                           3 + i, NULL));
    IHS_BufferInit(&datagram, 2048, 2048);
    char line[8192], raw[4096];
    unsigned id, sent;
    while (fgets(line, sizeof(line), stdin)) {
        if (sscanf(line, "P %u %u %4095s", &id, &sent, raw) == 3) {
            packet(5, id, sent, raw);
            session->base.callbacks.received(&session->base, &session->info.address, &datagram);
            IHS_BufferClear(&datagram, false);
            continue;
        }
        if (line[0] == 'N') {
            IHS_SessionNetworkUpdateAt(session, now);
            report();
            continue;
        }
        unsigned actionCode;
        assert(sscanf(line, "B %u %u", &now, &actionCode) == 2);
        if (actionCode == 2)
            IHS_SessionTransportDisconnect(session);
        readable = actionCode == 1;
        if (readable)
            packet(9, 0, 900000, "00");
        IHS_UDPPacket rx = {.buffer = IHS_BUFFER_INIT(2048, 2048)};
        session->base.callbacks.run->receiveCycle(&session->base, NULL, &rx);
        IHS_BufferClear(&rx.buffer, true);
        report();
    }
    IHS_BufferClear(&datagram, true);
    IHS_SessionDestroy(session);
    IHS_Quit();
    return 0;
}
