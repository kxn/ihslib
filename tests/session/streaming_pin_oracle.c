/* Actual request owner/timer/UDP and AES. IV and post-send secret replacement
 * are controlled inputs; no encryption result is replaced in positive cases. */
#include "client/client_pri.h"
#include "crypto.h"
#include "ihslib.h"
#include "network_test_discovery_peer.h"
#include <arpa/inet.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
/* Controlled wall milliseconds only; real monotonic time drives RX waits. */
static atomic_uint_fast64_t tick;
static atomic_uint encryptions;
static bool crypto_failure;
time_t __wrap_time(time_t *out) { time_t n=atomic_load(&tick)/1000; if(out)*out=n; return n; }
uint32_t __wrap_IHS_CryptoRandomUInt32(void) {
    return 17;
}
int __wrap_IHS_CryptoSymmetricEncrypt(const uint8_t *in, size_t n, const uint8_t *key, size_t k,
                                      uint8_t *out, size_t *size) {
    if (n != 8)
        atomic_fetch_add(&encryptions, 1);
    if (crypto_failure && n != 8) {
        memset(out, 0, *size); /* Defined output for the injected SDK failure. */
        return -1;
    }
    uint8_t iv[16];
    for (unsigned i = 0; i < 16; ++i)
        iv[i] = (uint8_t)i;
    return IHS_CryptoSymmetricEncryptWithIV(in, n, iv, 16, key, k, true, out, size);
}
static void noop(IHS_TimerTask *t, void *ctx) {
    (void)t;
    (void)ctx;
}
static void hex(const uint8_t *v, size_t n) {
    for (size_t i = 0; i < n; ++i)
        printf("%02x", v[i]);
}
static void receive(int fd) {
    struct pollfd p = {.fd = fd, .events = POLLIN};
    assert(poll(&p, 1, 2000) == 1);
    uint8_t raw[4096];
    ssize_t n = recv(fd, raw, sizeof(raw), 0);
    assert(n > 16);
    unsigned hs = test_discovery_u32(raw + 8), bytes = test_discovery_u32(raw + 12 + hs);
    assert(hs + bytes + 16u == (size_t)n);
    CMsgRemoteClientBroadcastHeader *h =
        cmsg_remote_client_broadcast_header__unpack(NULL, hs, raw + 12);
    assert(h);
    printf("{\"tag\":%d,\"pin\":\"", h->msg_type);
    if (h->msg_type == k_ERemoteDeviceStreamingRequest) {
        CMsgRemoteDeviceStreamingRequest *r =
            cmsg_remote_device_streaming_request__unpack(NULL, bytes, raw + 16 + hs);
        assert(r && r->request_id == 17 && r->has_pin);
        hex(r->pin.data, r->pin.len);
        cmsg_remote_device_streaming_request__free_unpacked(r, NULL);
    } else
        assert(h->msg_type == k_ERemoteDeviceStreamingCancelRequest);
    printf("\",\"encryptions\":%u}\n", atomic_load(&encryptions));
    cmsg_remote_client_broadcast_header__free_unpacked(h, NULL);
}
int main(int argc, char **argv) {
    assert(argc == 3 && strlen(argv[1]) <= 16);
    crypto_failure = atoi(argv[2]) != 0;
    atomic_store(&tick, 1000000);
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
    IHS_ClientConfig cfg = {
        .deviceId = 0x1122334455667788ULL, .secretKey = secret, .deviceName = "PIN oracle"};
    IHS_Client *c = IHS_ClientCreate(&cfg);
    assert(c);
    bool ready = false;
    for (unsigned i = 0; i < 2000 && !ready; ++i) {
        IHS_BaseLock(&c->base);
        ready = c->base.socket != NULL;
        IHS_BaseUnlock(&c->base);
        if (!ready)
            poll(NULL, 0, 1);
    }
    assert(ready);
    atomic_store(&encryptions, 0);
    IHS_HostInfo host = {.clientId = 123, .address.port = ntohs(peer.sin_port)};
    assert(IHS_IPAddressFromString(&host.address.ip, "127.0.0.1"));
    IHS_StreamingRequest r = {0};
    memcpy(r.pin, argv[1], strlen(argv[1]));
    bool started = IHS_ClientStreamingRequest(c, &host, &r);
    printf("{\"started\":%s}\n", started ? "true" : "false");
    if (started) {
        atomic_store(&tick, 1000025);
        receive(fd);
        assert(IHS_TimerTaskVisitOwned(c->timers, &c->taskHandles.streaming, noop, NULL));
        for (unsigned i = 0; i < 32; ++i)
            secret[i] = (uint8_t)(0xa0 + i);
        IHS_ClientSetSecretKey(c, secret);
        atomic_store(&tick, 1001025);
        receive(fd);
        assert(IHS_TimerTaskVisitOwned(c->timers, &c->taskHandles.streaming, noop, NULL));
    } else {
        receive(fd);
        IHS_BaseLock(&c->base);
        assert(!c->lastLaunchId);
        IHS_BaseUnlock(&c->base);
    }
    IHS_ClientStop(c);
    IHS_ClientThreadedJoin(c);
    IHS_ClientDestroy(c);
    close(fd);
    IHS_Quit();
    return 0;
}
