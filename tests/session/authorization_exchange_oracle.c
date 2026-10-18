/* Actual client RX/timer workers, discovery protobuf, crypto and UDP output.
 * Only random private input and optional producer failure/clock are controlled. */
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
static atomic_uint admitted, derives, successes, failures, progress, persist_calls;
static unsigned persist_at_confirm[64];
static atomic_uint confirm_count;
static atomic_int failure_result;
static bool rsa_failure, virtual_clock;
static atomic_uint_fast64_t virtual_now;
static atomic_int_fast64_t virtual_wall;
time_t __wrap_time(time_t *out) {
    time_t now = (time_t)atomic_load(&virtual_wall);
    if (out)
        *out = now;
    return now;
}
static unsigned tags[64], results[64], messages;
static char bodies[64][1024];
static struct sockaddr_in receiver;
static int peer;
int __real_clock_gettime(clockid_t, struct timespec *);
int __wrap_clock_gettime(clockid_t id, struct timespec *t) {
    if (virtual_clock && id == CLOCK_MONOTONIC) {
        uint64_t now = atomic_load(&virtual_now);
        t->tv_sec = now / 1000;
        t->tv_nsec = now % 1000 * 1000000;
        return 0;
    }
    return __real_clock_gettime(id, t);
}
static uint64_t millis(void) {
    struct timespec t;
    assert(!__real_clock_gettime(CLOCK_MONOTONIC, &t));
    return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
int __wrap_IHS_CryptoRandomBytes(uint8_t *out, size_t n) {
    for (size_t i = 0; i < n; ++i)
        out[i] = (uint8_t)i;
    return 0;
}
uint32_t __wrap_IHS_CryptoRandomUInt32(void) {
    return 17;
}
int __real_IHS_CryptoKeyExchange(const uint8_t *, const uint8_t *, uint8_t *);
int __wrap_IHS_CryptoKeyExchange(const uint8_t *private, const uint8_t *public, uint8_t *secret) {
    atomic_fetch_add(&derives, 1);
    return __real_IHS_CryptoKeyExchange(private, public, secret);
}
int __real_IHS_CryptoRSAEncrypt(const uint8_t *, size_t, const uint8_t *, size_t, uint8_t *,
                                size_t *);
int __wrap_IHS_CryptoRSAEncrypt(const uint8_t *in, size_t n, const uint8_t *key, size_t k,
                                uint8_t *out, size_t *size) {
    return rsa_failure ? -1 : __real_IHS_CryptoRSAEncrypt(in, n, key, k, out, size);
}
void __real_IHS_ClientAuthorizationCallback(IHS_Client *, const IHS_SocketAddress *,
                                            CMsgRemoteClientBroadcastHeader *, ProtobufCMessage *);
void __wrap_IHS_ClientAuthorizationCallback(IHS_Client *c, const IHS_SocketAddress *a,
                                            CMsgRemoteClientBroadcastHeader *h,
                                            ProtobufCMessage *m) {
    __real_IHS_ClientAuthorizationCallback(c, a, h, m);
    atomic_fetch_add(&admitted, 1);
}
bool __real_IHS_ClientSend(IHS_Client *, IHS_SocketAddress, ERemoteClientBroadcastMsg,
                           ProtobufCMessage *);
bool __wrap_IHS_ClientSend(IHS_Client *c, IHS_SocketAddress a, ERemoteClientBroadcastMsg tag,
                           ProtobufCMessage *m) {
    if (tag == k_ERemoteDeviceAuthorizationConfirmed) {
        unsigned n = atomic_load(&confirm_count);
        assert(n < 64);
        persist_at_confirm[n] = atomic_load(&persist_calls);
        atomic_store_explicit(&confirm_count, n + 1, memory_order_release);
    }
    return __real_IHS_ClientSend(c, a, tag, m);
}
static void persisted(IHS_Client *c, const IHS_HostInfo *h, uint64_t id, const uint8_t secret[32],
                      void *ctx) {
    (void)h;
    (void)ctx;
    assert(id == 76561198000000001ULL);
    uint8_t selected[32];
    IHS_ClientGetSecretKey(c, selected);
    assert(!memcmp(secret, selected, 32));
    atomic_fetch_add(&persist_calls, 1);
}
static unsigned queued(IHS_Client *c) {
    IHS_BaseLock(&c->base);
    unsigned n = c->authorizationCount;
    IHS_BaseUnlock(&c->base);
    return n;
}
static void success(IHS_Client *c, const IHS_HostInfo *h, uint64_t id, void *ctx) {
    (void)c;
    (void)h;
    (void)ctx;
    assert(id == 76561198000000001ULL);
    atomic_fetch_add(&successes, 1);
}
static void failed(IHS_Client *c, const IHS_HostInfo *h, IHS_AuthorizationResult result,
                   void *ctx) {
    (void)c;
    (void)h;
    (void)ctx;
    atomic_store(&failure_result, result);
    atomic_fetch_add(&failures, 1);
}
static void progressed(IHS_Client *c, const IHS_HostInfo *h, void *ctx) {
    (void)c;
    (void)h;
    (void)ctx;
    atomic_fetch_add(&progress, 1);
}
static void hex(const uint8_t *data, size_t n) {
    for (size_t i = 0; i < n; ++i)
        printf("%02x", data[i]);
}
static void drain(unsigned wait) {
    struct pollfd p = {.fd = peer, .events = POLLIN};
    while (poll(&p, 1, wait) > 0) {
        uint8_t raw[4096];
        socklen_t size = sizeof(receiver);
        ssize_t n = recvfrom(peer, raw, sizeof(raw), 0, (void *)&receiver, &size);
        assert(n > 16);
        unsigned hs = test_discovery_u32(raw + 8), body = test_discovery_u32(raw + 12 + hs);
        assert(hs + body + 16u == (size_t)n && messages < 64);
        CMsgRemoteClientBroadcastHeader *header =
            cmsg_remote_client_broadcast_header__unpack(NULL, hs, raw + 12);
        assert(header);
        unsigned at = messages++;
        tags[at] = header->msg_type;
        const uint8_t *bytes = raw + 16 + hs;
        if (tags[at] == k_ERemoteDeviceAuthorizationRequest) {
            CMsgRemoteDeviceAuthorizationRequest *r =
                cmsg_remote_device_authorization_request__unpack(NULL, body, bytes);
            assert(r && r->has_auth_key && r->has_request_id && r->request_id == 17);
            /* RSA payload/token and model metadata are distinct platform inputs. */
            assert(r->auth_key.len < sizeof(bodies[at]) / 2);
            for (size_t j = 0; j < r->auth_key.len; ++j)
                sprintf(bodies[at] + 2 * j, "%02x", r->auth_key.data[j]);
            cmsg_remote_device_authorization_request__free_unpacked(r, NULL);
        } else {
            assert(tags[at] == k_ERemoteDeviceAuthorizationConfirmed ||
                   tags[at] == k_ERemoteDeviceAuthorizationCancelRequest);
            assert(body < sizeof(bodies[at]) / 2);
            for (unsigned j = 0; j < body; ++j)
                sprintf(bodies[at] + 2 * j, "%02x", bytes[j]);
            if (tags[at] == k_ERemoteDeviceAuthorizationConfirmed) {
                CMsgRemoteDeviceAuthorizationConfirmed *r =
                    cmsg_remote_device_authorization_confirmed__unpack(NULL, body, bytes);
                assert(r);
                results[at] = r->result;
                cmsg_remote_device_authorization_confirmed__free_unpacked(r, NULL);
            }
        }
        cmsg_remote_client_broadcast_header__free_unpacked(header, NULL);
        wait = 5;
    }
}
static void noop(IHS_TimerTask *t, void *ctx) {
    (void)t;
    (void)ctx;
}
static void snapshot(IHS_Client *c) {
    drain(10);
    bool pending = IHS_TimerTaskVisitOwned(c->timers, &c->taskHandles.authorization, noop, NULL);
    uint64_t until = millis() + 2000;
    while (pending && (atomic_load(&successes) || atomic_load(&failures))) {
        assert(millis() < until);
        poll(NULL, 0, 1);
        pending = IHS_TimerTaskVisitOwned(c->timers, &c->taskHandles.authorization, noop, NULL);
    }
    uint8_t secret[32];
    IHS_ClientGetSecretKey(c, secret);
    printf("{\"pending\":%s,\"derived\":%u,\"success\":%u,\"failed\":%u,\"failure_result\":%d,"
           "\"progress\":%u,\"secret\":\"",
           pending ? "true" : "false", atomic_load(&derives), atomic_load(&successes),
           atomic_load(&failures), atomic_load(&failure_result), atomic_load(&progress));
    hex(secret, 32);
    printf("\",\"messages\":[");
    for (unsigned i = 0; i < messages; ++i)
        printf("%s{\"tag\":%u,\"body\":\"%s\"}", i ? "," : "", tags[i], bodies[i]);
    printf("],\"persist\":%u,\"persist_at_confirm\":[", atomic_load(&persist_calls));
    unsigned count = atomic_load_explicit(&confirm_count, memory_order_acquire);
    for (unsigned n = 0; n < count; ++n)
        printf("%s%u", n ? "," : "", persist_at_confirm[n]);
    puts("]}");
}
static void send_response(const uint8_t *body, size_t bytes) {
    CMsgRemoteClientBroadcastHeader h = CMSG_REMOTE_CLIENT_BROADCAST_HEADER__INIT;
    h.has_client_id = h.has_msg_type = true;
    h.client_id = 123;
    h.msg_type = k_ERemoteDeviceAuthorizationResponse;
    IHS_Buffer wire = IHS_BUFFER_INIT(1024, 0);
    IHS_BufferAppendMem(&wire, test_discovery_magic, 8);
    IHS_BufferAppendUInt32LE(&wire, cmsg_remote_client_broadcast_header__get_packed_size(&h));
    IHS_BufferAppendMessage(&wire, (void *)&h);
    IHS_BufferAppendUInt32LE(&wire, bytes);
    IHS_BufferAppendMem(&wire, body, bytes);
    assert(sendto(peer, IHS_BufferPointer(&wire), wire.size, 0, (void *)&receiver,
                  sizeof(receiver)) == (ssize_t)wire.size);
    IHS_BufferClear(&wire, true);
}
int main(int argc, char **argv) {
    assert(argc >= 2);
    rsa_failure = !strcmp(argv[1], "rsa");
    virtual_clock = !strcmp(argv[1], "retry") || !strcmp(argv[1], "wall_clock") ||
                    !strcmp(argv[1], "wall_rollback");
    atomic_store(&virtual_now, !strcmp(argv[1], "wall_clock") ? 1000900 : 1000000);
    atomic_store(&virtual_wall, 1000);
    IHS_Init();
    peer = socket(AF_INET, SOCK_DGRAM, 0);
    assert(peer >= 0);
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(!bind(peer, (void *)&address, sizeof(address)));
    socklen_t size = sizeof(address);
    assert(!getsockname(peer, (void *)&address, &size));
    uint8_t secret[32];
    for (unsigned n = 0; n < 32; ++n)
        secret[n] = 32 + n;
    IHS_ClientConfig config = {
        .deviceId = 0x1122334455667788ULL, .secretKey = secret, .deviceName = "Switch fixture"};
    IHS_Client *c = IHS_ClientCreate(&config);
    assert(c);
    IHS_ClientAuthorizationCallbacks cb = {
        .success = success, .failed = failed, .progress = progressed, .persist = persisted};
    IHS_ClientSetAuthorizationCallbacks(c, &cb, NULL);
    bool deferred =
        !strcmp(argv[1], "pump") || !strcmp(argv[1], "drop") || !strcmp(argv[1], "queue_full");
    if (deferred)
        IHS_ClientSetAuthorizationDeferred(c, true);
    bool ready = false;
    uint64_t until = millis() + 2000;
    while (!ready) {
        assert(millis() < until);
        IHS_BaseLock(&c->base);
        ready = c->base.socket != NULL;
        IHS_BaseUnlock(&c->base);
        if (!ready)
            poll(NULL, 0, 1);
    }
    IHS_HostInfo host = {.clientId = 123,
                         .universe = !strcmp(argv[1], "universe") ? 0 : IHS_SteamUniversePublic,
                         .address.port = ntohs(address.sin_port)};
    assert(IHS_IPAddressFromString(&host.address.ip, "127.0.0.1"));
    assert(IHS_ClientAuthorizationRequest(c, &host, "0042"));
    drain(2000);
    assert(messages == 1);
    if (rsa_failure || !strcmp(argv[1], "universe")) {
        until = millis() + 2000;
        while (!atomic_load(&failures)) {
            assert(millis() < until);
            poll(NULL, 0, 1);
        }
    }
    snapshot(c);
    if (!strcmp(argv[1], "retry")) {
        const unsigned offsets[] = {0, 1000, 2000, 3000, 3000, 5000, 6000, 300000};
        for (unsigned n = 0; n < 8; ++n) {
            /* Taking the owner lock observes TaskExecute's published deadline. */
            assert(IHS_TimerTaskVisitOwned(c->timers, &c->taskHandles.authorization, noop, NULL));
            atomic_store(&virtual_wall, 1000 + offsets[n] / 1000);
            atomic_store(&virtual_now, 1000000 + offsets[n]);
            bool due = n == 3 || n == 6 || n == 7;
            drain(due ? 2000 : 25);
            snapshot(c);
        }
    } else if (virtual_clock) {
        const int64_t phase[][2] = {{1001000, 1001}, {1002000, 1002}, {1002999, 1002},
                                    {1003000, 1003}, {1003000, 1003}, {1006000, 1006}};
        const int64_t rollback[][2] = {
            {1000020, 999}, {1000040, 998}, {1000060, 1002}, {1000080, 1003}, {1000100, 1300}};
        bool back = !strcmp(argv[1], "wall_rollback");
        unsigned count = back ? 5 : 6;
        for (unsigned i = 0; i < count; ++i) {
            assert(IHS_TimerTaskVisitOwned(c->timers, &c->taskHandles.authorization, noop, NULL));
            int64_t mono = back ? rollback[i][0] : phase[i][0],
                    wall = back ? rollback[i][1] : phase[i][1];
            atomic_store(&virtual_wall, wall);
            atomic_store(&virtual_now, mono);
            bool due = back ? (i >= 3) : (i == 3 || i == 5);
            drain(due ? 2000 : 25);
            snapshot(c);
        }
    } else if (!strcmp(argv[1], "normal") || deferred) {
        for (int j = 2; j < argc; ++j) {
            size_t bytes = strlen(argv[j]) / 2;
            assert(bytes < 2048 && !(strlen(argv[j]) & 1));
            uint8_t data[2048];
            for (size_t n = 0; n < bytes; ++n) {
                unsigned b;
                assert(sscanf(argv[j] + 2 * n, "%2x", &b) == 1);
                data[n] = (uint8_t)b;
            }
            send_response(data, bytes);
            until = millis() + 2000;
            if (!strcmp(argv[1], "queue_full")) {
                for (unsigned n = 1; n < 33; ++n)
                    send_response(data, bytes);
                unsigned drops = 0;
                while (!drops) {
                    assert(millis() < until);
                    IHS_BaseLock(&c->base);
                    drops = c->authorizationDropped;
                    IHS_BaseUnlock(&c->base);
                    if (!drops)
                        poll(NULL, 0, 1);
                }
                assert(drops == 1 && queued(c) == 32 && !atomic_load(&admitted));
                assert(IHS_ClientPumpAuthorization(c, 1) == 1 && queued(c) == 31);
                assert(IHS_ClientPumpAuthorization(c, 31) == 31 && !queued(c));
                snapshot(c);
                break;
            }
            if (deferred) {
                while (!queued(c)) {
                    assert(millis() < until);
                    poll(NULL, 0, 1);
                }
                assert(atomic_load(&admitted) == (unsigned)(j - 2));
                if (!strcmp(argv[1], "drop"))
                    break;
                assert(IHS_ClientPumpAuthorization(c, 1) == 1);
            }
            while (atomic_load(&admitted) < (unsigned)(j - 1)) {
                assert(millis() < until);
                poll(NULL, 0, 1);
            }
            snapshot(c);
        }
    }
    IHS_ClientStop(c);
    IHS_ClientThreadedJoin(c);
    IHS_ClientDestroy(c);
    close(peer);
    IHS_Quit();
    return 0;
}
