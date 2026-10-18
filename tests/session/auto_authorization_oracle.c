/* Actual deferred actor, RX worker, wire parser, timer ownership, AES/RSA and
 * callbacks. Commands supply wall time, peer bytes, PIN/private/IV entropy. */
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
static atomic_int_fast64_t wall;
static unsigned keypairs, derives, saves, prompts, resumes, success_count, failures, iv_index;
static int failure_result;
static uint8_t accepted_key[32];
static size_t accepted_length;
static struct sockaddr_in destination;
static int peer;
static char bodies[64][4096];
static unsigned tags[64], saved_at_send[64], message_count;
static unsigned observed_tags[64], observed_saves[64], observed_count;
static uint8_t ticket_secrets[32][32];
static unsigned ticket_count;
time_t __wrap_time(time_t *out) {
    time_t n = atomic_load(&wall);
    if (out)
        *out = n;
    return n;
}
uint32_t __wrap_IHS_CryptoRandomUInt32(void) {
    return 17;
}
int __wrap_IHS_CryptoRandomBytes(uint8_t *out, size_t n) {
    if (n == 4) {
        uint32_t r = 42;
        memcpy(out, &r, 4);
    } else {
        if (n == 32)
            ++keypairs;
        for (size_t i = 0; i < n; ++i)
            out[i] = i;
    }
    return 0;
}
int __real_IHS_CryptoKeyExchange(const uint8_t *, const uint8_t *, uint8_t *);
int __wrap_IHS_CryptoKeyExchange(const uint8_t *a, const uint8_t *b, uint8_t *s) {
    ++derives;
    return __real_IHS_CryptoKeyExchange(a, b, s);
}
int __wrap_IHS_CryptoSymmetricEncrypt(const uint8_t *p, size_t n, const uint8_t *k, size_t z,
                                      uint8_t *out, size_t *size) {
    uint8_t iv[16];
    for (unsigned i = 0; i < 16; ++i)
        iv[i] = (uint8_t)(16 * iv_index + i);
    ++iv_index;
    return IHS_CryptoSymmetricEncryptWithIV(p, n, iv, 16, k, z, true, out, size);
}
int __real_IHS_CryptoRSAEncrypt(const uint8_t *, size_t, const uint8_t *, size_t, uint8_t *,
                                size_t *);
int __wrap_IHS_CryptoRSAEncrypt(const uint8_t *p, size_t n, const uint8_t *key, size_t k,
                                uint8_t *out, size_t *size) {
    CMsgRemoteDeviceAuthorizationRequest__CKeyEscrowTicket *t = (void *)protobuf_c_message_unpack(
        &cmsg_remote_device_authorization_request__ckey_escrow__ticket__descriptor, NULL, n, p);
    assert(t && t->has_payload && t->payload.len == 32 && t->password.len == 4 &&
           !memcmp(t->password.data, "0042", 4));
    assert(ticket_count < 32);
    memcpy(ticket_secrets[ticket_count++], t->payload.data, 32);
    protobuf_c_message_free_unpacked((void *)t, NULL);
    return __real_IHS_CryptoRSAEncrypt(p, n, key, k, out, size);
}
bool __real_IHS_ClientSend(IHS_Client *, IHS_SocketAddress, ERemoteClientBroadcastMsg,
                           ProtobufCMessage *);
bool __wrap_IHS_ClientSend(IHS_Client *c, IHS_SocketAddress address, ERemoteClientBroadcastMsg tag,
                           ProtobufCMessage *m) {
    assert(observed_count < 64);
    observed_tags[observed_count] = tag;
    observed_saves[observed_count++] = saves;
    return __real_IHS_ClientSend(c, address, tag, m);
}
static void persist(IHS_Client *c, const IHS_HostInfo *h, uint64_t id, const uint8_t s[32],
                    void *ctx) {
    (void)h;
    (void)id;
    (void)ctx;
    uint8_t selected[32];
    IHS_ClientGetSecretKey(c, selected);
    assert(!memcmp(s, selected, 32));
    ++saves;
}
static void authorization(IHS_Client *c, const IHS_HostInfo *h, const char *pin, uint64_t id,
                          void *ctx) {
    (void)c;
    (void)h;
    (void)id;
    (void)ctx;
    if (pin) {
        assert(!strcmp(pin, "0042"));
        ++prompts;
    } else
        ++resumes;
}
static void authorized_fail(IHS_Client *c, const IHS_HostInfo *h, IHS_AuthorizationResult r,
                            void *ctx) {
    (void)c;
    (void)h;
    (void)ctx;
    ++failures;
    failure_result = r;
}
static void failed(IHS_Client *c, const IHS_HostInfo *h, IHS_StreamingResult r, void *ctx) {
    (void)c;
    (void)h;
    (void)ctx;
    ++failures;
    failure_result = r;
}
static void accepted(IHS_Client *c, const IHS_HostInfo *h, const IHS_SocketAddress *a,
                     const uint8_t *key, size_t n, void *ctx) {
    (void)c;
    (void)h;
    (void)a;
    (void)ctx;
    assert(n == 16 || n == 32);
    memcpy(accepted_key, key, n);
    accepted_length = n;
    ++success_count;
}
static void hex(const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; ++i)
        printf("%02x", p[i]);
}
static size_t unhex(const char *p, uint8_t *out) {
    size_t n = strlen(p) / 2;
    assert(n < 2048 && !(strlen(p) & 1));
    for (size_t i = 0; i < n; ++i) {
        unsigned x;
        assert(sscanf(p + 2 * i, "%2x", &x) == 1);
        out[i] = x;
    }
    return n;
}
static void drain(void) {
    struct pollfd p = {.fd = peer, .events = POLLIN};
    while (poll(&p, 1, 5) > 0) {
        uint8_t wire[4096];
        socklen_t sz = sizeof(destination);
        ssize_t n = recvfrom(peer, wire, sizeof(wire), 0, (void *)&destination, &sz);
        assert(n > 16);
        unsigned hs = test_discovery_u32(wire + 8), bytes = test_discovery_u32(wire + 12 + hs);
        assert(hs + bytes + 16u == (size_t)n && message_count < 64 && bytes < 2048);
        CMsgRemoteClientBroadcastHeader *h =
            cmsg_remote_client_broadcast_header__unpack(NULL, hs, wire + 12);
        assert(h);
        unsigned at = message_count++;
        tags[at] = h->msg_type;
        assert(at < observed_count && observed_tags[at] == (unsigned)h->msg_type);
        saved_at_send[at] = observed_saves[at];
        for (unsigned i = 0; i < bytes; ++i)
            sprintf(bodies[at] + 2 * i, "%02x", wire[16 + hs + i]);
        cmsg_remote_client_broadcast_header__free_unpacked(h, NULL);
    }
}
static void snapshot(IHS_Client *c) {
    drain();
    uint8_t secret[32];
    IHS_ClientGetSecretKey(c, secret);
    IHS_BaseLock(&c->base);
    unsigned state = c->lastLaunchId ? (c->lastLaunchHasTicket     ? 6
                                        : c->lastLaunchAuthorizing ? 4
                                                                   : 5)
                                     : 1;
    printf("{\"state\":%u,\"rid\":%u,\"secret\":\"", state, c->lastLaunchId);
    hex(secret, 32);
    printf("\",\"cache\":\"");
    hex(c->authorizationCache.key, c->authorizationCache.keyLen);
    printf("\",\"cached_result\":%d,\"queued\":%u,\"messages\":[", c->authorizationCache.result,
           c->authorizationCount);
    IHS_BaseUnlock(&c->base);
    for (unsigned i = 0; i < message_count; ++i)
        printf("%s{\"tag\":%u,\"body\":\"%s\",\"saves\":%u}", i ? "," : "", tags[i], bodies[i],
               saved_at_send[i]);
    printf("],\"keypairs\":%u,\"derived\":%u,\"saves\":%u,\"prompts\":%u,\"resumes\":%u,"
           "\"successes\":%u,\"failures\":%u,\"failure_result\":%d,\"accepted\":\"",
           keypairs, derives, saves, prompts, resumes, success_count, failures, failure_result);
    hex(accepted_key, accepted_length);
    printf("\",\"tickets\":[");
    for (unsigned i = 0; i < ticket_count; ++i) {
        printf("%s\"", i ? "," : "");
        hex(ticket_secrets[i], 32);
        printf("\"");
    }
    puts("]}");
}
static void packet(IHS_Client *c, unsigned tag, uint64_t host, const char *body, bool dispatch) {
    IHS_BaseLock(&c->base);
    unsigned before = c->authorizationCount;
    IHS_BaseUnlock(&c->base);
    uint8_t bytes[2048];
    size_t n = unhex(body, bytes);
    CMsgRemoteClientBroadcastHeader h = CMSG_REMOTE_CLIENT_BROADCAST_HEADER__INIT;
    h.has_client_id = h.has_msg_type = true;
    h.client_id = host;
    h.msg_type = tag;
    IHS_Buffer wire = IHS_BUFFER_INIT(4096, 0);
    IHS_BufferAppendMem(&wire, test_discovery_magic, 8);
    IHS_BufferAppendUInt32LE(&wire, cmsg_remote_client_broadcast_header__get_packed_size(&h));
    IHS_BufferAppendMessage(&wire, (void *)&h);
    IHS_BufferAppendUInt32LE(&wire, n);
    IHS_BufferAppendMem(&wire, bytes, n);
    assert(sendto(peer, IHS_BufferPointer(&wire), wire.size, 0, (void *)&destination,
                  sizeof(destination)) == (ssize_t)wire.size);
    IHS_BufferClear(&wire, true);
    bool queued = false;
    for (unsigned i = 0; i < 2000 && !queued; ++i) {
        IHS_BaseLock(&c->base);
        queued = c->authorizationCount > before;
        IHS_BaseUnlock(&c->base);
        if (!queued)
            poll(NULL, 0, 1);
    }
    assert(queued);
    if (dispatch)
        IHS_ClientPumpAuthorization(c, 32);
}
int main(int argc, char **argv) {
    assert(argc == 2);
    atomic_store(&wall, 1000);
    IHS_Init();
    peer = socket(AF_INET, SOCK_DGRAM, 0);
    assert(peer >= 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(!bind(peer, (void *)&a, sizeof(a)));
    socklen_t size = sizeof(a);
    assert(!getsockname(peer, (void *)&a, &size));
    uint8_t secret[32];
    for (unsigned i = 0; i < 32; ++i)
        secret[i] = 32 + i;
    IHS_ClientConfig cfg = {
        .deviceId = 0x1122334455667788ULL, .secretKey = secret, .deviceName = "Switch fixture"};
    IHS_Client *c = IHS_ClientCreate(&cfg);
    assert(c);
    IHS_ClientSetAuthorizationDeferred(c, true);
    IHS_ClientAuthorizationCallbacks ac = {.persist = persist};
    IHS_ClientStreamingCallbacks sc = {.authorization = authorization,
                                       .authorizationFailed = authorized_fail,
                                       .success = accepted,
                                       .failed = failed};
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
    IHS_StreamingRequest request = {.networkTest = atoi(argv[1]) != 0};
    strcpy(request.pin, "9876");
    assert(IHS_ClientStreamingRequest(c, &host, &request));
    IHS_ClientPumpAuthorization(c, 32);
    snapshot(c);
    char line[8192];
    while (fgets(line, sizeof(line), stdin)) {
        char op;
        long long time;
        assert(sscanf(line, "%c %lld", &op, &time) == 2);
        atomic_store(&wall, time);
        if (op == 'p' || op == 'q') {
            unsigned tag;
            unsigned long long id;
            char body[4096];
            assert(sscanf(line, "%c %lld %u %llu %4095s", &op, &time, &tag, &id, body) == 5);
            packet(c, tag, id, body, op == 'p');
        } else if (op == 'b') {
            unsigned budget;
            assert(sscanf(line, "%c %lld %u", &op, &time, &budget) == 3);
            IHS_ClientPumpAuthorization(c, budget);
        } else if (op == 'u')
            IHS_ClientPumpAuthorization(c, 32);
        else {
            assert(op == 'c');
            IHS_ClientStreamingCancel(c);
        }
        snapshot(c);
    }
    IHS_ClientStop(c);
    IHS_ClientThreadedJoin(c);
    IHS_ClientDestroy(c);
    close(peer);
    IHS_Quit();
    return 0;
}
