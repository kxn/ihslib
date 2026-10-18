/* Production discovery producer/response parser, actual HMAC/AES and final UDP
 * sends; response admission is explicitly driven on the test thread (§120). */
#include "client/client_pri.h"
#include "ihslib.h"
#include "network_test_discovery_peer.h"
#include "session/session_pri.h"
#include <arpa/inet.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
static unsigned successes, failures, progress, auth_prompts;
static int failure_result;
static uint8_t received_key[128];
static size_t key_size;
static IHS_SocketAddress received_address;
static bool virtual_clock;
static atomic_uint_fast64_t virtual_now;
int __real_clock_gettime(clockid_t, struct timespec *);
int __wrap_clock_gettime(clockid_t clock, struct timespec *time) {
    if (virtual_clock && clock == CLOCK_MONOTONIC) {
        uint64_t now = atomic_load(&virtual_now);
        time->tv_sec = now / 1000;
        time->tv_nsec = (now % 1000) * 1000000;
        return 0;
    }
    return __real_clock_gettime(clock, time);
}
time_t __wrap_time(time_t *out) { time_t n = virtual_clock ? atomic_load(&virtual_now)/1000 : 1000; if(out)*out=n; return n; }
uint32_t __wrap_IHS_CryptoRandomUInt32(void) {
    return 17;
}
static void success(IHS_Client *c, const IHS_HostInfo *h, const IHS_SocketAddress *a,
                    const uint8_t *key, size_t count, void *ctx) {
    (void)c;
    (void)h;
    (void)ctx;
    assert(count <= sizeof(received_key));
    memcpy(received_key, key, count);
    key_size = count;
    received_address = *a;
    ++successes;
}
static void failure(IHS_Client *c, const IHS_HostInfo *h, IHS_StreamingResult result, void *ctx) {
    (void)c;
    (void)h;
    (void)ctx;
    ++failures;
    failure_result = result;
}
static void progressed(IHS_Client *c, const IHS_HostInfo *h, void *ctx) {
    (void)c;
    (void)h;
    (void)ctx;
    ++progress;
}
static void hex(const uint8_t *p, size_t count) {
    for (size_t i = 0; i < count; ++i)
        printf("%02x", p[i]);
}
static void completed_send(IHS_TimerTask *task, void *context) {
    (void)task; (void)context;
}
static void wait_send_return(IHS_Client *client) {
    /* A visitor acquires the owner lock after the send callback returns. Poll
     * run-count is not send-count now that deadlines are wall-clock predicates. */
    assert(IHS_TimerTaskVisitOwned(client->timers, &client->taskHandles.streaming,
                                  completed_send, NULL));
}
static void authorization(IHS_Client *c, const IHS_HostInfo *h, const char *pin, uint64_t id, void *ctx) {
    (void)c; (void)h; (void)id; (void)ctx;
    if(pin) { assert(strlen(pin)==4); ++auth_prompts; }
}
int main(int argc, char **argv) {
    assert(argc >= 4 && argc <= 6);
    virtual_clock = !strcmp(argv[3], "-");
    atomic_store(&virtual_now, 1000000);
    IHS_Init();
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    assert(fd >= 0);
    struct sockaddr_in peer = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(!bind(fd, (void *)&peer, sizeof(peer)));
    socklen_t len = sizeof(peer);
    assert(!getsockname(fd, (void *)&peer, &len));
    uint8_t secret[32];
    for (unsigned i = 0; i < 32; ++i)
        secret[i] = 32 + i;
    IHS_ClientConfig config = {
        .deviceId = 1, .secretKey = secret, .deviceName = "discovery oracle"};
    IHS_Client *c = IHS_ClientCreate(&config);
    assert(c);
    /* Advancing the simulated clock must follow actual socket publication;
     * otherwise the first send can precede RX worker setup and the frozen
     * deadline would correctly defer its retry forever in this fixture. */
    bool ready = false;
    for (unsigned i = 0; !ready && i < 2000; ++i) {
        IHS_BaseLock(&c->base);
        ready = c->base.socket != NULL;
        IHS_BaseUnlock(&c->base);
        if (!ready)
            poll(NULL, 0, 1);
    }
    assert(ready);
    IHS_HostInfo host = {.clientId = 123, .universe = IHS_SteamUniversePublic, .address.port = ntohs(peer.sin_port)};
    assert(IHS_IPAddressFromString(&host.address.ip, "127.0.0.1"));
    IHS_ClientStreamingCallbacks cb = {
        .success = success, .failed = failure, .progress = progressed, .authorization = authorization};
    if (argc == 6) {
        cb.success = NULL;
        if (atoi(argv[5]) == 2)
            cb.failed = NULL;
    }
    IHS_ClientSetStreamingCallbacks(c, &cb, NULL);
    IHS_StreamingRequest req = {.networkTest = atoi(argv[1]),
                                .gamepadCount = atoi(argv[2]),
                                .maxResolution = {1280, 720},
                                .audioChannelCount = 2,
                                .streamingEnable = {true, true, true},
                                .streamingInterface = IHS_StreamInterfaceBigPicture};
    assert(IHS_ClientStreamingRequest(c, &host, &req));
    if (virtual_clock)
        atomic_store(&virtual_now, 1000025);
    struct pollfd wait = {.fd = fd, .events = POLLIN};
    assert(poll(&wait, 1, 2000) == 1);
    uint8_t request[4096];
    ssize_t size = recv(fd, request, sizeof(request), 0);
    assert(size > 16);
    unsigned hs = test_discovery_u32(request + 8);
    assert(hs + 16u <= (size_t)size);
    unsigned count = test_discovery_u32(request + 12 + hs);
    assert(count + hs + 16u == (size_t)size);
    if (virtual_clock) {
        const uint64_t offsets[] = {0, 1000, 1000, 2000, 15000, 300000, 85400000};
        unsigned sends = 1;
        wait_send_return(c);
        printf("{\"sends\":[");
        for (unsigned i = 0; i < 7; ++i) {
            atomic_store(&virtual_now, 1000025 + offsets[i]);
            struct pollfd waiter = {.fd = fd, .events = POLLIN};
            bool due = i != 0 && i != 2;
            int ready = poll(&waiter, 1, due ? 2000 : 30);
            assert(ready == (due ? 1 : 0));
            if (ready) {
                assert(recv(fd, request, sizeof(request), 0) > 16);
                ++sends;
                wait_send_return(c);
            }
            printf("%s%u", i ? "," : "", sends);
        }
        printf("],\"failures\":%u}\n", failures);
        IHS_ClientStop(c);
        IHS_ClientThreadedJoin(c);
        IHS_ClientDestroy(c);
        close(fd);
        IHS_Quit();
        return 0;
    }
    uint8_t response[2048];
    size_t bytes = strlen(argv[3]) / 2;
    assert(bytes <= sizeof(response));
    for (size_t i = 0; i < bytes; ++i) {
        unsigned b;
        assert(sscanf(argv[3] + 2 * i, "%2x", &b) == 1);
        response[i] = b;
    }
    /* Feed the original source body's protobuf through the actual discovery
     * envelope parser; production RX socket scheduling is a separate test. */
    CMsgRemoteClientBroadcastHeader header = CMSG_REMOTE_CLIENT_BROADCAST_HEADER__INIT;
    header.has_client_id = header.has_msg_type = true;
    header.client_id = 123;
    header.msg_type = k_ERemoteDeviceStreamingResponse;
    IHS_Buffer wire = IHS_BUFFER_INIT(1024, 0);
    IHS_BufferAppendMem(&wire, test_discovery_magic, 8);
    IHS_BufferAppendUInt32LE(&wire, cmsg_remote_client_broadcast_header__get_packed_size(&header));
    IHS_BufferAppendMessage(&wire, (const ProtobufCMessage *)&header);
    IHS_BufferAppendUInt32LE(&wire, bytes);
    IHS_BufferAppendMem(&wire, response, bytes);
    /* BaseWorker dispatches discovery callbacks without holding Base. Follow
     * that ownership order: the timer visitor can subsequently acquire Base. */
    for (unsigned i = 0; i < (argc >= 5 ? (unsigned)atoi(argv[4]) : 1); ++i) {
        IHS_Buffer copy = wire;
        c->base.callbacks.received(&c->base, &host.address, &copy);
    }
    IHS_BufferClear(&wire, true);
    bool configured = false;
    uint8_t hmac[32];
    size_t hmac_count = sizeof(hmac);
    if (successes && key_size <= 32) {
        IHS_SessionInfo info = {.address = received_address, .sessionKeyLen = key_size};
        memcpy(info.sessionKey, received_key, key_size);
        IHS_Session *s = IHS_SessionCreate(&config, &info);
        assert(s);
        configured = IHS_SessionConfigureNetworkTest(s, IHS_NetworkTestFixed, 20000);
        if (configured) {
            static const uint8_t plain[] = "Steam In-Home Streaming";
            assert(!IHS_SessionFrameHMACSHA256(s, plain, sizeof(plain) - 1, hmac, &hmac_count));
        }
        IHS_SessionDestroy(s);
    }
    printf("{\"request\":\"");
    hex(request + 16 + hs, count);
    printf("\",\"success\":%u,\"failures\":%u,\"failure_result\":%d,\"progress\":%u,\"port\":%u,"
           "\"key\":\"",
           successes, failures, failure_result, progress, received_address.port);
    hex(received_key, key_size);
    printf("\",\"authorizing\":%s,\"configured\":%s,\"hmac\":\"", auth_prompts ? "true" : "false", configured ? "true" : "false");
    if (configured)
        hex(hmac, hmac_count);
    printf("\",\"cancels\":[");
    unsigned cancels = 0;
    uint8_t data[4096];
    ssize_t n;
    while ((n = recv(fd, data, sizeof(data), MSG_DONTWAIT)) > 0) {
        assert(n > 16);
        unsigned head = test_discovery_u32(data + 8);
        assert(head + 16u <= (size_t)n);
        CMsgRemoteClientBroadcastHeader *h =
            cmsg_remote_client_broadcast_header__unpack(NULL, head, data + 12);
        assert(h);
        if (h->msg_type == k_ERemoteDeviceStreamingCancelRequest) {
            printf("%s\"", cancels++ ? "," : "");
            hex(data + 16 + head, test_discovery_u32(data + 12 + head));
            printf("\"");
        }
        cmsg_remote_client_broadcast_header__free_unpacked(h, NULL);
    }
    printf("]}\n");
    IHS_ClientStop(c);
    IHS_ClientThreadedJoin(c);
    IHS_ClientDestroy(c);
    close(fd);
    IHS_Quit();
    return 0;
}
