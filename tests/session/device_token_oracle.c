/* Actual producers/timers/UDP and AES/RSA. Identity, IV and secret-change steps
 * are controlled inputs; a failed encrypt is a defined SDK failure provider. */
#include "client/client_pri.h"
#include "crypto.h"
#include "ihslib.h"
#include "network_test_discovery_peer.h"
#include <arpa/inet.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
static atomic_uint_fast64_t tick;
static atomic_int_fast64_t wall;
static atomic_uint step, attempts, encryptions, pin_encryptions, failures;
static atomic_int failure_code;
static _Thread_local bool in_token;
static uint8_t ivs[16][16], secrets[16][32], ticket_secret[32];
static bool failed_step[16], have_ticket;
static pthread_mutex_t observer = PTHREAD_MUTEX_INITIALIZER;
int __real_clock_gettime(clockid_t, struct timespec *);
int __wrap_clock_gettime(clockid_t id, struct timespec *t) {
    if (id == CLOCK_MONOTONIC) {
        uint64_t n = atomic_load(&tick);
        t->tv_sec = n / 1000;
        t->tv_nsec = n % 1000 * 1000000;
        return 0;
    }
    return __real_clock_gettime(id, t);
}
time_t __wrap_time(time_t *p) {
    time_t n = atomic_load(&wall);
    if (p)
        *p = n;
    return n;
}
uint32_t __wrap_IHS_CryptoRandomUInt32(void) {
    return 17;
}
bool __real_IHS_ClientCreateDeviceToken(IHS_Client *, const uint8_t *, uint8_t *);
bool __wrap_IHS_ClientCreateDeviceToken(IHS_Client *c, const uint8_t *secret, uint8_t *out) {
    atomic_fetch_add(&attempts, 1);
    in_token = true;
    bool result = __real_IHS_ClientCreateDeviceToken(c, secret, out);
    in_token = false;
    return result;
}
int __wrap_IHS_CryptoSymmetricEncrypt(const uint8_t *in, size_t n, const uint8_t *key, size_t k,
                                      uint8_t *out, size_t *size) {
    uint8_t iv[16];
    unsigned index = atomic_load(&step);
    if (in_token) {
        atomic_fetch_add(&encryptions, 1);
        if (failed_step[index]) {
            memset(out, 0, *size);
            return -1;
        }
        memcpy(iv, ivs[index], 16);
    } else {
        atomic_fetch_add(&pin_encryptions, 1);
        for (unsigned i = 0; i < 16; ++i)
            iv[i] = i;
    }
    return IHS_CryptoSymmetricEncryptWithIV(in, n, iv, 16, key, k, true, out, size);
}
int __real_IHS_CryptoRSAEncrypt(const uint8_t *, size_t, const uint8_t *, size_t, uint8_t *,
                                size_t *);
int __wrap_IHS_CryptoRSAEncrypt(const uint8_t *in, size_t n, const uint8_t *key, size_t k,
                                uint8_t *out, size_t *size) {
    CMsgRemoteDeviceAuthorizationRequest__CKeyEscrowTicket *t = (void *)protobuf_c_message_unpack(
        &cmsg_remote_device_authorization_request__ckey_escrow__ticket__descriptor, NULL, n, in);
    assert(t && t->has_payload && t->payload.len == 32);
    pthread_mutex_lock(&observer);
    memcpy(ticket_secret, t->payload.data, 32);
    have_ticket = true;
    pthread_mutex_unlock(&observer);
    protobuf_c_message_free_unpacked((void *)t, NULL);
    return __real_IHS_CryptoRSAEncrypt(in, n, key, k, out, size);
}
static void auth_failed(IHS_Client *c, const IHS_HostInfo *h, IHS_AuthorizationResult e,
                        void *ctx) {
    (void)c;
    (void)h;
    (void)ctx;
    atomic_store(&failure_code, e);
    atomic_fetch_add(&failures, 1);
}
static void stream_failed(IHS_Client *c, const IHS_HostInfo *h, IHS_StreamingResult e, void *ctx) {
    (void)c;
    (void)h;
    (void)ctx;
    atomic_store(&failure_code, e);
    atomic_fetch_add(&failures, 1);
}
static void noop(IHS_TimerTask *t, void *ctx) {
    (void)t;
    (void)ctx;
}
static void hex(const uint8_t *v, size_t n) {
    for (size_t i = 0; i < n; ++i)
        printf("%02x", v[i]);
}
static void unhex(const char *v, uint8_t *out, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        unsigned b;
        assert(sscanf(v + 2 * i, "%2x", &b) == 1);
        out[i] = b;
    }
}
static void receive(IHS_Client *c, int fd, bool auth) {
    struct pollfd p = {.fd = fd, .events = POLLIN};
    assert(poll(&p, 1, 2000) == 1);
    uint8_t wire[4096];
    ssize_t n = recv(fd, wire, sizeof(wire), 0);
    assert(n > 16);
    unsigned hs = test_discovery_u32(wire + 8), count = test_discovery_u32(wire + 12 + hs);
    assert(hs + count + 16u == (size_t)n);
    CMsgRemoteClientBroadcastHeader *h =
        cmsg_remote_client_broadcast_header__unpack(NULL, hs, wire + 12);
    assert(h);
    bool pending = IHS_TimerTaskVisitOwned(
        c->timers, auth ? &c->taskHandles.authorization : &c->taskHandles.streaming, noop, NULL);
    printf("{\"tag\":%d,\"pending\":%s,\"token\":\"", h->msg_type, pending ? "true" : "false");
    if (h->msg_type == k_ERemoteDeviceStreamingRequest) {
        CMsgRemoteDeviceStreamingRequest *r =
            cmsg_remote_device_streaming_request__unpack(NULL, count, wire + 16 + hs);
        assert(r && r->has_device_token && r->has_pin && r->request_id == 17);
        hex(r->device_token.data, r->device_token.len);
        printf("\",\"pin\":\"");
        hex(r->pin.data, r->pin.len);
        printf("\",\"test\":%s", r->network_test ? "true" : "false");
        cmsg_remote_device_streaming_request__free_unpacked(r, NULL);
    } else if (h->msg_type == k_ERemoteDeviceAuthorizationRequest) {
        CMsgRemoteDeviceAuthorizationRequest *r =
            cmsg_remote_device_authorization_request__unpack(NULL, count, wire + 16 + hs);
        assert(r && r->has_request_id && r->request_id == 17);
        hex(r->device_token.data, r->device_token.len);
        printf("\",\"pin\":\"\",\"test\":false");
        cmsg_remote_device_authorization_request__free_unpacked(r, NULL);
    } else {
        assert(h->msg_type == k_ERemoteDeviceStreamingCancelRequest ||
               h->msg_type == k_ERemoteDeviceAuthorizationCancelRequest);
        printf("\",\"pin\":\"\",\"test\":false");
    }
    printf(",\"body\":\"");
    hex(wire + 16 + hs, count);
    printf("\",\"attempts\":%u,\"encryptions\":%u,\"pin_encryptions\":%u,\"failed\":%u,\"failure_"
           "code\":%d,\"ticket_secret\":\"",
           atomic_load(&attempts), atomic_load(&encryptions), atomic_load(&pin_encryptions),
           atomic_load(&failures), atomic_load(&failure_code));
    pthread_mutex_lock(&observer);
    if (have_ticket)
        hex(ticket_secret, 32);
    pthread_mutex_unlock(&observer);
    puts("\"}");
    cmsg_remote_client_broadcast_header__free_unpacked(h, NULL);
}
int main(int argc, char **argv) {
    assert(argc >= 6 && argc <= 21 && strlen(argv[3]) <= 16);
    bool auth = !strcmp(argv[1], "auth");
    unsigned count = argc - 5;
    for (unsigned i = 0; i < count; ++i) {
        char *colon = strchr(argv[i + 5], ':');
        assert(colon == argv[i + 5] + 64);
        unhex(argv[i + 5], secrets[i], 32);
        failed_step[i] = colon[1] == '!';
        if (!failed_step[i]) {
            assert(strlen(colon + 1) == 32);
            unhex(colon + 1, ivs[i], 16);
        }
    }
    atomic_store(&tick, 1000000);
    atomic_store(&wall, 1000);
    IHS_Init();
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    assert(fd >= 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(!bind(fd, (void *)&a, sizeof(a)));
    socklen_t size = sizeof(a);
    assert(!getsockname(fd, (void *)&a, &size));
    IHS_ClientConfig config = {.deviceId = strtoull(argv[2], NULL, 10),
                               .secretKey = secrets[0],
                               .deviceName = "Token oracle"};
    IHS_Client *c = IHS_ClientCreate(&config);
    assert(c);
    IHS_ClientAuthorizationCallbacks ac = {.failed = auth_failed};
    IHS_ClientStreamingCallbacks sc = {.failed = stream_failed};
    IHS_ClientSetAuthorizationCallbacks(c, &ac, NULL);
    IHS_ClientSetStreamingCallbacks(c, &sc, NULL);
    bool ready = false;
    for (unsigned i = 0; i < 2000 && !ready; ++i) {
        IHS_BaseLock(&c->base);
        ready = c->base.socket != NULL;
        IHS_BaseUnlock(&c->base);
        if (!ready)
            poll(NULL, 0, 1);
    }
    assert(ready);
    IHS_HostInfo host = {
        .clientId = 123, .universe = IHS_SteamUniversePublic, .address.port = ntohs(a.sin_port)};
    assert(IHS_IPAddressFromString(&host.address.ip, "127.0.0.1"));
    IHS_StreamingRequest request = {.networkTest = atoi(argv[4]) != 0};
    memcpy(request.pin, argv[3], strlen(argv[3]));
    assert(auth ? IHS_ClientAuthorizationRequest(c, &host, "0042")
                : IHS_ClientStreamingRequest(c, &host, &request));
    for (unsigned i = 0; i < count; ++i) {
        atomic_store(&step, i);
        if (i && memcmp(secrets[i], secrets[i - 1], 32))
            IHS_ClientSetSecretKey(c, secrets[i]);
        atomic_store(&wall, 1000 + i * 3);
        atomic_store(&tick, 1000025 + i * (auth ? 3000 : 1000));
        receive(c, fd, auth);
    }
    IHS_ClientStop(c);
    IHS_ClientThreadedJoin(c);
    IHS_ClientDestroy(c);
    close(fd);
    IHS_Quit();
    return 0;
}
