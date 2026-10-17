/* Real shutdown, callback timing, reply CRC and ingress. Only final I/O, time
 * and the process-wide next-ID value are controlled. No worker/Steam host. */
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

static uint32_t now = 1000000;
static unsigned result, appNotices;
static bool applicationClose;
static char actions[8192];
static int udpError;
static uint32_t randomValues[16];
static unsigned randomCount, randomCalls;
uint32_t __real_IHS_CryptoRandomUInt32(void);
uint8_t __real_IHS_SessionNextConnectionId(void);
uint32_t __wrap_IHS_CryptoRandomUInt32(void) {
    if (!randomCount)
        return __real_IHS_CryptoRandomUInt32();
    assert(randomCalls < randomCount);
    return randomValues[randomCalls++];
}
uint32_t __wrap_IHS_SessionPacketTimestamp(void) {
    return now;
}
uint8_t __wrap_IHS_SessionNextConnectionId(void) {
    return 12;
}
bool __wrap_IHS_BaseSend(IHS_Base *b, IHS_SocketAddress addr, const IHS_Buffer *wire) {
    (void)b;
    (void)addr;
    size_t offset = strlen(actions);
    offset +=
        (size_t)sprintf(actions + offset, "%s{\"action\":\"send\",\"wire\":\"", offset ? "," : "");
    assert(offset + wire->size * 2 + 4 < sizeof(actions));
    for (size_t i = 0; i < wire->size; ++i)
        sprintf(actions + offset + 2 * i, "%02x", IHS_BufferPointer(wire)[i]);
    strcat(actions, "\"}");
    errno = udpError ? udpError : result == 1 ? EAGAIN : EIO;
    return !result && !udpError;
}
static void disconnected(IHS_Session *s, void *ctx) { (void)s; (void)ctx; ++appNotices; }
void __real_IHS_SessionClientTransportClosed(IHS_Session *);
void __wrap_IHS_SessionClientTransportClosed(IHS_Session *s) {
    size_t offset = strlen(actions);
    snprintf(actions + offset, sizeof(actions) - offset,
             "%s{\"action\":\"closed\",\"state\":%d,\"local\":%u,\"peer\":%u}", offset ? "," : "",
             IHS_SessionTransportState(s), (unsigned)s->state.connectionId,
             (unsigned)s->state.hostConnectionId);
    __real_IHS_SessionClientTransportClosed(s);
}
static void print(IHS_Session *s) {
    printf("{\"state\":%d,\"last_connect\":%u,\"actions\":[%s],\"crc_required\":%s,"
           "\"attached\":%s,\"delegate\":%s,\"local\":%u,\"peer\":%u,\"last_receive\":%u,\"app_"
           "notices\":%u}\n",
           IHS_SessionTransportState(s), s->transport.lastConnect, actions,
           s->state.crcRequired ? "true" : "false", s->transport.detached ? "false" : "true",
           s->transport.detached ? "false" : "true", (unsigned)s->state.connectionId,
           (unsigned)s->state.hostConnectionId, s->transport.lastReceive, appNotices);
    actions[0] = 0;
}
int main(int argc, char **argv) {
    if (argc >= 5 && !strcmp(argv[1], "IDS")) {
        unsigned warmup = (unsigned)atoi(argv[2]), count = (unsigned)atoi(argv[3]);
        assert((unsigned)argc - 4 <= 16);
        randomCount = (unsigned)argc - 4;
        for (unsigned i = 0; i < randomCount; ++i)
            randomValues[i] = (uint32_t)strtoul(argv[i + 4], NULL, 0);
        for (unsigned i = 0; i < warmup; ++i)
            __real_IHS_SessionNextConnectionId();
        printf("{\"ids\":[");
        for (unsigned i = 0; i < count; ++i)
            printf("%s%u", i ? "," : "", __real_IHS_SessionNextConnectionId());
        printf("],\"random_calls\":%u}\n", randomCalls);
        return 0;
    }
    assert(argc == 4);
    IHS_Init();
    IHS_Session *s = IHS_TestSessionCreate();
    IHS_TimerTaskStopImmediate(s->retransmission.timer);
    int state = atoi(argv[1]);
    s->state.connectionState = state == 0   ? IHS_SessionConnectionStateConnecting
                               : state == 3 ? IHS_SessionConnectionStateUnconnected
                                            : IHS_SessionConnectionStateConnected;
    atomic_store(&s->transport.shutdown, state >= 2 ? state - 1 : 0);
    s->state.connectionId = 12;
    s->state.hostConnectionId = 34;
    s->state.crcRequired = atoi(argv[2]) != 0;
    result = (unsigned)atoi(argv[3]);
    IHS_SessionTransportBegin(s, now);
    const IHS_StreamSessionCallbacks cb = {.disconnected = disconnected};
    char line[512], hex[512];
    unsigned matches, value;
    while (fgets(line, sizeof(line), stdin)) {
        if (sscanf(line, "UDP %u", &value) == 1) {
            const int errors[] = {0, EAGAIN, EMSGSIZE, EBADF, EIO, EINTR};
            assert(value < 6);
            udpError = errors[value];
            IHS_SessionPacket packet = {.header = {.type = IHS_SessionPacketTypeDisconnect,
                                                   .srcConnectionId = 12,
                                                   .dstConnectionId = 34}};
            IHS_SessionPacketBodyInitialize(&packet.body, false);
            int sent = IHS_SessionSendPacketResult(s, &packet);
            IHS_SessionPacketClear(&packet, true);
            printf("{\"result\":%d,\"actions\":[%s]}\n", sent, actions);
            actions[0] = 0;
            continue;
        } else if (!strncmp(line, "DISCONNECT", 10)) {
            IHS_SessionTransportDisconnect(s);
        } else if (!strncmp(line, "CLOSE", 5)) {
            IHS_SessionTransportClose(s);
        } else if (!strncmp(line, "CLIENT", 6)) {
            applicationClose = true;
            IHS_SessionSetSessionCallbacks(s, &cb, NULL);
            IHS_SessionDisconnect(s);
        } else if (sscanf(line, "UPDATE %u", &now) == 1) {
            s->base.callbacks.run->update(&s->base, NULL);
        } else if (sscanf(line, "WIRE %u %u %511s", &now, &matches, hex) == 3) {
            size_t length = strlen(hex) / 2;
            IHS_Buffer data = IHS_BUFFER_INIT(512, 512);
            for (size_t i = 0; i < length; ++i) {
                assert(sscanf(hex + 2 * i, "%2x", &value) == 1);
                IHS_BufferAppendUInt8(&data, (uint8_t)value);
            }
            IHS_SocketAddress address = s->info.address;
            if (!matches)
                address.port++;
            s->base.callbacks.received(&s->base, &address, &data);
            IHS_BufferClear(&data, true);
        } else {
            assert(!"unknown command");
        }
        print(s);
    }
    IHS_SessionDestroy(s);
    IHS_Quit();
    return 0;
}
