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
static atomic_uint received_count;
static atomic_bool hold_update, update_held;
static _Atomic(IHS_Cond *) receive_condition;
bool __real_IHS_CondWaitTimeout(IHS_Cond *, IHS_Mutex *, uint32_t);
bool __wrap_IHS_CondWaitTimeout(IHS_Cond *cond, IHS_Mutex *mutex, uint32_t ms) {
    if (cond == atomic_load(&receive_condition) && atomic_load(&hold_update)) {
        /* Explicit idle-gap gate. Release Base so runtime-style pumps can
         * run, then restore ownership before forwarding the actual wait. */
        IHS_MutexUnlock(mutex);
        atomic_store(&update_held, true);
        while (atomic_load(&hold_update)) poll(NULL, 0, 1);
        IHS_MutexLock(mutex);
        atomic_store(&update_held, false);
    }
    return __real_IHS_CondWaitTimeout(cond, mutex, ms);
}
static IHS_BaseReceivedFunction *original_received;
static void received(IHS_Base *b, const IHS_SocketAddress *a, IHS_Buffer *wire) {
    original_received(b, a, wire);
    atomic_fetch_add_explicit(&received_count, 1, memory_order_release);
}
void __real_IHS_BaseInit(IHS_Base *, const IHS_ClientConfig *, IHS_BaseReceivedFunction *, bool);
void __wrap_IHS_BaseInit(IHS_Base *b, const IHS_ClientConfig *cfg, IHS_BaseReceivedFunction *fn,
                         bool broadcast) {
    original_received = fn;
    __real_IHS_BaseInit(b, cfg, received, broadcast);
}
static _Thread_local bool in_proof;
static bool fail_proof_encrypt;
static struct {
    const char *op;
    char plain[8192], secret[65];
    uint64_t client, steam;
    unsigned tag;
} proof_ops[128];
static unsigned proof_count, random_id;
static void text_hex(char *out, const uint8_t *p, size_t n) {
    assert(n * 2 < 8192);
    for (size_t i = 0; i < n; ++i)
        sprintf(out + 2 * i, "%02x", p[i]);
}
int __real_IHS_CryptoSHA256(const uint8_t *, size_t, uint8_t *);
int __wrap_IHS_CryptoSHA256(const uint8_t *p, size_t n, uint8_t *out) {
    if (in_proof) {
        assert(proof_count < 128);
        proof_ops[proof_count].op = "hash";
        text_hex(proof_ops[proof_count++].plain, p, n);
    }
    return __real_IHS_CryptoSHA256(p, n, out);
}
void __real_IHS_ClientStreamingCallback(IHS_Client *, const IHS_SocketAddress *,
                                        CMsgRemoteClientBroadcastHeader *, ProtobufCMessage *);
void __wrap_IHS_ClientStreamingCallback(IHS_Client *c, const IHS_SocketAddress *a,
                                        CMsgRemoteClientBroadcastHeader *h, ProtobufCMessage *m) {
    in_proof = h->msg_type == k_ERemoteDeviceProofRequest;
    __real_IHS_ClientStreamingCallback(c, a, h, m);
    in_proof = false;
}
static unsigned keypairs, derives, saves, prompts, resumes, success_count, failures, iv_index;
static bool service_preparing;
static bool service_oracle, service_hint = true;
static unsigned service_random_call, service_draw_count, publication_count,
    service_notification_count;
static uint8_t service_draws[64][32];
static char publications[16][8192], ticket_wires[32][4096];
static struct {
    const char *kind;
    uint64_t client;
    unsigned error, state, rid;
} service_notifications[32];
static unsigned handshake_state(IHS_Client *c) {
    return c->servicePairing          ? 3
           : !c->lastLaunchId         ? 1
           : c->lastLaunchHasTicket   ? 6
           : c->lastLaunchAuthorizing ? 4
                                      : 5;
}
static int failure_result;
static uint8_t accepted_key[32];
static size_t accepted_length;
static struct sockaddr_in destination;
static int peer;
static char bodies[64][4096];
static unsigned tags[64], saved_at_send[64], message_count;
static unsigned observed_tags[64], observed_saves[64], observed_count;
static struct { const char *op; uint64_t client, steam; unsigned tag; } metadata_ops[192];
static unsigned metadata_count;
static void metadata_op(const char *op, uint64_t client, uint64_t steam, unsigned tag) {
    assert(metadata_count < 192);
    metadata_ops[metadata_count].op = op;
    metadata_ops[metadata_count].client = client;
    metadata_ops[metadata_count].steam = steam;
    metadata_ops[metadata_count++].tag = tag;
}
static void identity(IHS_Client *c, uint64_t client, uint64_t steam, void *ctx) {
    (void)c; (void)ctx;
    metadata_op("identity", client, steam, 0);
}
static void subscribe(IHS_Client *c, uint64_t steam, void *ctx) {
    (void)c; (void)ctx;
    metadata_op("subscribe", 0, steam, 0);
}
static uint8_t ticket_secrets[32][32];
static unsigned ticket_count;
static struct {
    uint64_t client, steam;
    char secret[65];
} saved_keys[64];
static IHS_HostInfo route_host;
static bool route_visible(IHS_Client *client, uint64_t id, IHS_HostInfo *out, void *ctx) {
    (void)client;
    (void)ctx;
    if (service_oracle && IHS_ClientAuthorizationServiceActive(client)) {
        if (!service_hint)
            return false;
        *out = route_host;
        out->clientId = id;
        return true; /* Explicit Source GetServerInfo/hint provider, not observed reachability. */
    }
    if (id != route_host.clientId)
        return false;
    *out = route_host;
    return true;
}
static void route_address(IHS_Client *client, uint64_t id, IHS_SocketAddress *out, void *ctx) {
    (void)client;
    (void)ctx;
    *out = id == route_host.clientId ? route_host.address : (IHS_SocketAddress){0};
}
static const IHS_ClientRoutingCallbacks routing = {.host = route_visible, .resolve = route_address};
static struct {
    unsigned tag, state, rid, failures;
    int result;
    char cache[8192], secret[65];
} send_states[64];
static struct {
    unsigned state, rid, failures;
    int result;
    char cache[8192], secret[65];
} persist_states[64];
time_t __wrap_time(time_t *out) {
    time_t n = atomic_load(&wall);
    if (out)
        *out = n;
    return n;
}
uint32_t __wrap_IHS_CryptoRandomUInt32(void) {
    const unsigned ids[] = {17, 19, 23, 29};
    assert(random_id < 4);
    return ids[random_id++];
}
int __wrap_IHS_CryptoRandomBytes(uint8_t *out, size_t n) {
    if (service_preparing && n == 32 && service_random_call++ > 0) {
        assert(service_draw_count < 64);
        for (unsigned i = 0; i < 32; ++i)
            out[i] = (uint8_t)(i + (service_random_call == 2 ? 0 : 64));
        memcpy(service_draws[service_draw_count++], out, 32);
        return 0;
    }
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
    if (in_proof) {
        assert(proof_count < 128 && z == 32);
        proof_ops[proof_count].op = "encrypt";
        text_hex(proof_ops[proof_count].plain, p, n);
        text_hex(proof_ops[proof_count++].secret, k, z);
        if (fail_proof_encrypt)
            return -1;
    }
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
    assert(t && t->has_payload && t->payload.len == 32 && t->password.len == 4);
    assert(ticket_count < 32);
    assert(n < sizeof(ticket_wires[0]) / 2);
    text_hex(ticket_wires[ticket_count], p, n);
    memcpy(ticket_secrets[ticket_count++], t->payload.data, 32);
    protobuf_c_message_free_unpacked((void *)t, NULL);
    return __real_IHS_CryptoRSAEncrypt(p, n, key, k, out, size);
}
static void observe_send(IHS_Client *client, ERemoteClientBroadcastMsg tag) {
    metadata_op("send", 0, 0, tag);
    if (in_proof) {
        assert(proof_count < 128);
        proof_ops[proof_count].op = "send";
        proof_ops[proof_count++].tag = tag;
    }
    assert(observed_count < 64);
    IHS_BaseLock(&client->base);
    unsigned n = observed_count;
    send_states[n].tag = tag;
    send_states[n].state = handshake_state(client);
    send_states[n].rid = client->lastLaunchId;
    send_states[n].failures = failures;
    send_states[n].result = client->authorizationCache.result;
    text_hex(send_states[n].cache, client->authorizationCache.key,
             client->authorizationCache.keyLen);
    text_hex(send_states[n].secret, client->base.secretKey, 32);
    IHS_BaseUnlock(&client->base);
    observed_tags[observed_count] = tag;
    observed_saves[observed_count++] = saves;
}
bool __real_IHS_ClientSend(IHS_Client *, IHS_SocketAddress, ERemoteClientBroadcastMsg,
                           ProtobufCMessage *);
static void observe_call(ERemoteClientBroadcastMsg tag, ProtobufCMessage *m) {
    if (!service_oracle)
        return;
    assert(message_count < 64);
    unsigned at = message_count++;
    size_t size = m ? protobuf_c_message_get_packed_size(m) : 0;
    assert(size < sizeof(bodies[0]) / 2);
    uint8_t raw[2048];
    if (m)
        protobuf_c_message_pack(m, raw);
    text_hex(bodies[at], raw, size);
    tags[at] = tag;
    saved_at_send[at] = saves;
}
bool __wrap_IHS_ClientSend(IHS_Client *c, IHS_SocketAddress address, ERemoteClientBroadcastMsg tag,
                           ProtobufCMessage *m) {
    observe_send(c, tag);
    observe_call(tag, m);
    return __real_IHS_ClientSend(c, address, tag, m);
}
bool __real_IHS_ClientSendControl(IHS_Client *, const IHS_HostInfo *, const IHS_SocketAddress *,
                                  ERemoteClientBroadcastMsg, ProtobufCMessage *);
bool __wrap_IHS_ClientSendControl(IHS_Client *c, const IHS_HostInfo *host,
                                  const IHS_SocketAddress *source, ERemoteClientBroadcastMsg tag,
                                  ProtobufCMessage *m) {
    observe_send(c, tag);
    observe_call(tag, m);
    return __real_IHS_ClientSendControl(c, host, source, tag, m);
}
static void persist(IHS_Client *c, const IHS_HostInfo *h, uint64_t id, const uint8_t s[32],
                    void *ctx) {
    (void)ctx;
    uint8_t selected[32];
    metadata_op("key", h->clientId, id, 0);
    IHS_ClientGetSecretKey(c, selected);
    assert(!memcmp(s, selected, 32));
    assert(saves < 64);
    IHS_BaseLock(&c->base);
    persist_states[saves].state = handshake_state(c);
    persist_states[saves].rid = c->lastLaunchId;
    persist_states[saves].failures = failures;
    persist_states[saves].result = c->authorizationCache.result;
    text_hex(persist_states[saves].cache, c->authorizationCache.key, c->authorizationCache.keyLen);
    text_hex(persist_states[saves].secret, selected, 32);
    IHS_BaseUnlock(&c->base);
    saved_keys[saves].client = h->clientId;
    saved_keys[saves].steam = id;
    text_hex(saved_keys[saves].secret, s, 32);
    ++saves;
}
static void persist_proof(IHS_Client *c, const IHS_HostInfo *h, uint64_t id, const uint8_t key[32],
                          void *ctx) {
    assert(in_proof && proof_count < 128);
    proof_ops[proof_count].op = "save";
    proof_ops[proof_count].client = h->clientId;
    proof_ops[proof_count].steam = id;
    text_hex(proof_ops[proof_count++].secret, key, 32);
    persist(c, h, id, key, ctx);
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
static void service_notification(IHS_Client *c, const char *kind, uint64_t client, unsigned error) {
    assert(service_notification_count < 32);
    IHS_BaseLock(&c->base);
    unsigned i = service_notification_count++;
    service_notifications[i].kind = kind;
    service_notifications[i].client = client;
    service_notifications[i].error = error;
    service_notifications[i].state = handshake_state(c);
    service_notifications[i].rid = c->lastLaunchId;
    IHS_BaseUnlock(&c->base);
}
static void service_authorized(IHS_Client *c, const IHS_HostInfo *h, uint64_t account, void *ctx) {
    (void)account;
    (void)ctx;
    service_notification(c, "paired", h->clientId, 0);
}
static void service_failed(IHS_Client *c, const IHS_HostInfo *h, IHS_AuthorizationResult result,
                           void *ctx) {
    authorized_fail(c, h, result, ctx);
    unsigned error = result == IHS_AuthorizationDenied        ? 2
                     : result == IHS_AuthorizationNotLoggedIn ? 3
                     : result == IHS_AuthorizationOffline     ? 4
                     : result == IHS_AuthorizationBusy        ? 5
                     : result == IHS_AuthorizationTimedOut    ? 6
                                                              : 1;
    service_notification(c, "failed", 0, error);
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
        if (service_oracle) {
            cmsg_remote_client_broadcast_header__free_unpacked(h, NULL);
            continue;
        }
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
    unsigned state = handshake_state(c);
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
    printf("],\"ticket_wires\":[");
    for (unsigned i = 0; i < ticket_count; ++i)
        printf("%s\"%s\"", i ? "," : "", ticket_wires[i]);
    printf("],\"publications\":[");
    for (unsigned i = 0; i < publication_count; ++i)
        printf("%s\"%s\"", i ? "," : "", publications[i]);
    printf("],\"service_draws\":[");
    for (unsigned i = 0; i < service_draw_count; ++i) {
        printf("%s\"", i ? "," : "");
        hex(service_draws[i], 32);
        putchar('"');
    }
    printf("],\"service_notifications\":[");
    for (unsigned i = 0; i < service_notification_count; ++i)
        printf("%s{\"kind\":\"%s\",\"client\":%llu,\"error\":%u,\"state\":%u,\"rid\":%u}",
               i ? "," : "", service_notifications[i].kind,
               (unsigned long long)service_notifications[i].client, service_notifications[i].error,
               service_notifications[i].state, service_notifications[i].rid);
    printf("],\"send_states\":[");
    for (unsigned i = 0; i < observed_count; ++i) {
        printf("%s{\"tag\":%u,\"state\":%u,\"rid\":%u,\"cache\":\"%s\","
               "\"cached_result\":%d,\"failures\":%u",
               i ? "," : "", send_states[i].tag, send_states[i].state, send_states[i].rid,
               send_states[i].cache, send_states[i].result, send_states[i].failures);
        if (service_oracle)
            printf(",\"secret\":\"%s\"", send_states[i].secret);
        putchar('}');
    }
    printf("],\"persist_states\":[");
    for (unsigned i = 0; i < saves; ++i) {
        printf("%s{\"state\":%u,\"rid\":%u,\"cache\":\"%s\","
               "\"cached_result\":%d,\"failures\":%u",
               i ? "," : "", persist_states[i].state, persist_states[i].rid,
               persist_states[i].cache, persist_states[i].result, persist_states[i].failures);
        if (service_oracle)
            printf(",\"secret\":\"%s\"", persist_states[i].secret);
        putchar('}');
    }
    printf("],\"saved_keys\":[");
    for (unsigned i = 0; i < saves; ++i)
        printf("%s{\"client\":%llu,\"steamid\":%llu,\"secret\":\"%s\"}", i ? "," : "",
               (unsigned long long)saved_keys[i].client, (unsigned long long)saved_keys[i].steam,
               saved_keys[i].secret);
    printf("],\"metadata_ops\":[");
    for (unsigned i = 0; i < metadata_count; ++i)
        printf("%s{\"op\":\"%s\",\"client\":%llu,\"steam\":%llu,\"tag\":%u}", i ? "," : "",
               metadata_ops[i].op, (unsigned long long)metadata_ops[i].client,
               (unsigned long long)metadata_ops[i].steam, metadata_ops[i].tag);
    printf("],\"proof_ops\":[");
    for (unsigned i = 0; i < proof_count; ++i) {
        printf("%s{\"op\":\"%s\",\"plain\":\"%s\",\"secret\":\"%s\",\"client\":%llu,\"steamid\":%"
               "llu,\"tag\":%u}",
               i ? "," : "", proof_ops[i].op, proof_ops[i].plain, proof_ops[i].secret,
               (unsigned long long)proof_ops[i].client, (unsigned long long)proof_ops[i].steam,
               proof_ops[i].tag);
    }
    puts("]}");
}
static void packet(IHS_Client *c, unsigned tag, uint64_t host, const char *body,
                   bool dispatch, bool wait) {
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
    if (!wait) return;
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
    assert(argc >= 2 && argc <= 4);
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
    IHS_ClientConfig cfg = {.deviceId = 0x1122334455667788ULL,
                            .secretKey = secret,
                            .deviceName = "Switch fixture",
                            .deviceModel = "Switch model fixture",
                            .deviceSerial = "serial fixture"};
    IHS_Client *c = IHS_ClientCreate(&cfg);
    assert(c);
    atomic_store(&receive_condition, c->receiveWake);
    IHS_ClientSetAuthorizationDeferred(c, true);
    IHS_ClientAuthorizationCallbacks ac = {.persist = persist,
                                           .identity = identity,
                                           .subscribe = subscribe,
                                           .success = service_authorized,
                                           .failed = service_failed};
    IHS_ClientStreamingCallbacks sc = {.authorization = authorization,
                                       .authorizationFailed = authorized_fail,
                                       .success = accepted,
                                       .failed = failed,
                                       .persistProof = persist_proof};
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
    bool service_mode = argc == 4 && !strcmp(argv[3], "service");
    service_oracle =
        service_mode; /* Service comparison observes caller sends, like its original fixture. */
    if (argc == 4) {
        assert(service_mode || !strcmp(argv[3], "routed"));
        route_host = host;
        IHS_ClientSetRoutingCallbacks(c, &routing, NULL);
    }
    if (service_mode) {
        /* Learn the real ephemeral UDP endpoint without starting an unrelated
         * streaming transaction. This byte is a fixture socket bootstrap. */
        uint8_t byte = 0;
        IHS_Buffer bootstrap = IHS_BUFFER_WRAP(&byte, 1);
        assert(IHS_BaseSend(&c->base, host.address, &bootstrap));
        socklen_t length = sizeof(destination);
        assert(recvfrom(peer, &byte, 1, 0, (void *)&destination, &length) == 1);
    }
    IHS_StreamingRequest request = {.networkTest = atoi(argv[1]) != 0,
                                    .keySteamId = argc == 3 ? strtoull(argv[2], NULL, 10) : 0};
    strcpy(request.pin, "9876");
    if (!service_mode)
        assert(IHS_ClientStreamingRequest(c, &host, &request));
    /* Native has a separate RX owner; source's initial Start cannot interrupt
     * an in-progress manager Update. Let that real batch finish if needed. */
    for (unsigned n = 0; !service_mode && n < 2000 && !observed_count; ++n) {
        IHS_ClientPumpAuthorization(c, 32);
        if (!observed_count) poll(NULL, 0, 1);
    }
    assert(service_mode || observed_count);
    snapshot(c);
    char line[8192];
    while (fgets(line, sizeof(line), stdin)) {
        char op;
        long long time;
        assert(sscanf(line, "%c %lld", &op, &time) == 2);
        atomic_store(&wall, time);
        if (op == 'p' || op == 'q' || op == 'd' || op == 'w') {
            unsigned tag;
            unsigned long long id;
            char body[4096];
            assert(sscanf(line, "%c %lld %u %llu %4095s", &op, &time, &tag, &id, body) == 5);
            if (op == 'd') {
                uint8_t raw[2048];
                size_t size = unhex(body, raw);
                ProtobufCMessage *m = protobuf_c_message_unpack(
                    &cmsg_remote_device_proof_request__descriptor, NULL, size, raw);
                assert(m && tag == 7);
                CMsgRemoteClientBroadcastHeader h = CMSG_REMOTE_CLIENT_BROADCAST_HEADER__INIT;
                h.has_client_id = h.has_msg_type = true;
                h.client_id = id;
                h.msg_type = tag;
                IHS_ClientStreamingCallback(c, &host.address, &h, m);
                protobuf_c_message_free_unpacked(m, NULL);
            } else
                packet(c, tag, id, body, op == 'p', op != 'w');
        } else if (op == 'r') {
            char body[4096];
            assert(sscanf(line, "%c %lld %4095s", &op, &time, body) == 3);
            uint8_t raw[2048];
            size_t size = unhex(body, raw);
            unsigned before = atomic_load_explicit(&received_count, memory_order_acquire);
            assert(sendto(peer, raw, size, 0, (void *)&destination, sizeof(destination)) ==
                   (ssize_t)size);
            bool done = false;
            for (unsigned n = 0; n < 2000 && !done; ++n) {
                done = atomic_load_explicit(&received_count, memory_order_acquire) > before;
                if (!done)
                    poll(NULL, 0, 1);
            }
            assert(done);
            IHS_ClientPumpAuthorization(c, 32);
        } else if (op == 'f') {
            unsigned failed;
            assert(sscanf(line, "%c %lld %u", &op, &time, &failed) == 3);
            fail_proof_encrypt = failed != 0;
        } else if (op == 'a') {
            char pin[16];
            unsigned override;
            int args = sscanf(line, "%c %lld %15s %u", &op, &time, pin, &override);
            assert(args == 3 || args == 4);
            if (args == 4) {
                IHS_BaseLock(&c->base);
                c->lastLaunchId = override;
                IHS_BaseUnlock(&c->base);
            }
            uint8_t *body = NULL;
            size_t size = 0;
            service_preparing = true;
            service_random_call = 0;
            assert(IHS_ClientAuthorizationPrepareService(c, IHS_SteamUniversePublic, pin, &body,
                                                         &size));
            service_preparing = false;
            assert(publication_count < 16 && size < sizeof(publications[0]) / 2);
            text_hex(publications[publication_count++], body, size);
            memset(body, 0, size);
            free(body);
            service_notification(c, "code", 0, 0);
        } else if (op == 'z') {
            IHS_ClientAuthorizationCancel(c);
        } else if (op == 'h') {
            unsigned enabled;
            assert(sscanf(line, "%c %lld %u", &op, &time, &enabled) == 3 && enabled <= 1);
            service_hint = enabled != 0;
        } else if (op == 'k') {
            strcpy(request.pin, "9876");
            assert(IHS_ClientStreamingRequest(c, &host, &request));
            IHS_ClientPumpAuthorization(c, 32);
        } else if (op == 's') {
            request.pin[0] = 0; /* §133 replacement Start receives empty PIN. */
            assert(IHS_ClientStreamingRequest(c, &host, &request));
            IHS_ClientPumpAuthorization(c, 32);
        } else if (op == 'b') {
            unsigned budget;
            assert(sscanf(line, "%c %lld %u", &op, &time, &budget) == 3);
            IHS_ClientPumpAuthorization(c, budget);
        } else if (op == 'g') {
            unsigned hold;
            assert(sscanf(line, "%c %lld %u", &op, &time, &hold) == 3 && hold <= 1);
            IHS_BaseLock(&c->base);
            uint64_t before = c->receiveGeneration;
            IHS_BaseUnlock(&c->base);
            atomic_store(&hold_update, hold != 0);
            bool complete = false;
            for (unsigned n = 0; n < 2000 && !complete; ++n) {
                if (hold) complete = atomic_load(&update_held);
                else {
                    IHS_BaseLock(&c->base);
                    complete = !c->receiveBatchActive && c->receiveGeneration != before;
                    IHS_BaseUnlock(&c->base);
                }
                if (!complete) poll(NULL, 0, 1);
            }
            assert(complete);
            if (!hold) IHS_ClientPumpAuthorization(c, 32);
        } else if (op == 'n') {
            IHS_ClientPumpAuthorization(c, 32); /* no new Update requested */
        } else if (op == 'u') {
            /* Source 'u' is a complete manager Update, not another pump in
             * the idle gap between socket polls. Wait for the actual receive
             * owner to publish a new completed batch; never forge its epoch. */
            IHS_BaseLock(&c->base);
            uint64_t before = c->receiveGeneration;
            IHS_BaseUnlock(&c->base);
            bool complete = false;
            for (unsigned n = 0; n < 2000 && !complete; ++n) {
                IHS_BaseLock(&c->base);
                complete = !c->receiveBatchActive && c->receiveGeneration != before;
                IHS_BaseUnlock(&c->base);
                if (!complete) poll(NULL, 0, 1);
            }
            assert(complete);
            IHS_ClientPumpAuthorization(c, 32);
        } else {
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
