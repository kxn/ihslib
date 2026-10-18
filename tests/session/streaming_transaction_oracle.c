/* Real client/timer/UDP output and real discovery parser/AES; inbound admission
 * is synchronously driven, following BaseWorker's no-Base-lock call order. */
#include "client/client_pri.h"
#include "crypto.h"
#include "ihslib.h"
#include "network_test_discovery_peer.h"
#include <arpa/inet.h>
#include <assert.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static unsigned random_index, message_count;
static struct {
    unsigned tag, id, target;
    bool test;
    char body[128];
} messages[32];
static int sockets[2];
static IHS_HostInfo hosts[2];
static IHS_Client *client;
uint32_t __wrap_IHS_CryptoRandomUInt32(void) {
    return 17 + 2 * random_index++;
}
static void drain(unsigned timeout) {
    struct pollfd p[2] = {{.fd = sockets[0], .events = POLLIN},
                          {.fd = sockets[1], .events = POLLIN}};
    while (poll(p, 2, timeout) > 0) {
        for (unsigned i = 0; i < 2; ++i)
            if (p[i].revents & POLLIN) {
                uint8_t wire[4096];
                ssize_t n = recv(sockets[i], wire, sizeof(wire), 0);
                assert(n > 16);
                unsigned hs = test_discovery_u32(wire + 8),
                         size = test_discovery_u32(wire + 12 + hs);
                assert(hs + 16u + size == (size_t)n);
                CMsgRemoteClientBroadcastHeader *h =
                    cmsg_remote_client_broadcast_header__unpack(NULL, hs, wire + 12);
                assert(h);
                assert(message_count < 32);
                unsigned at = message_count++;
                messages[at].tag = h->msg_type;
                messages[at].target = hosts[i].clientId;
                const uint8_t *body = wire + 16 + hs;
                if (h->msg_type == k_ERemoteDeviceStreamingRequest) {
                    CMsgRemoteDeviceStreamingRequest *r =
                        cmsg_remote_device_streaming_request__unpack(NULL, size, body);
                    assert(r && r->has_network_test);
                    messages[at].id = r->request_id;
                    messages[at].test = r->network_test;
                    cmsg_remote_device_streaming_request__free_unpacked(r, NULL);
                } else {
                    assert(h->msg_type == k_ERemoteDeviceStreamingCancelRequest && size < 64);
                    CMsgRemoteDeviceStreamingCancelRequest *r =
                        cmsg_remote_device_streaming_cancel_request__unpack(NULL, size, body);
                    assert(r);
                    messages[at].id = r->request_id;
                    for (unsigned j = 0; j < size; ++j)
                        sprintf(messages[at].body + j * 2, "%02x", body[j]);
                    cmsg_remote_device_streaming_cancel_request__free_unpacked(r, NULL);
                }
                cmsg_remote_client_broadcast_header__free_unpacked(h, NULL);
            }
        timeout = 15;
    }
}
static void snapshot(const char *label) {
    drain(20);
    IHS_BaseLock(&client->base);
    unsigned id = client->lastLaunchId, state = id ? (client->lastLaunchHasTicket ? 6 : 5) : 1;
    unsigned target = id ? client->lastLaunchHost.clientId : 0;
    IHS_BaseUnlock(&client->base);
    printf("{\"label\":\"%s\",\"state\":%u,\"request_id\":%u,\"target\":%u,\"messages\":[", label,
           state, id, target);
    for (unsigned i = 0; i < message_count; ++i) {
        printf("%s{\"tag\":%u,\"request_id\":%u,\"target\":%u", i ? "," : "", messages[i].tag,
               messages[i].id, messages[i].target);
        if (messages[i].tag == 5)
            printf(",\"network_test\":%s", messages[i].test ? "true" : "false");
        else
            printf(",\"body\":\"%s\"", messages[i].body);
        printf("}");
    }
    puts("]}");
}
static void start(bool test, unsigned host) {
    IHS_StreamingRequest r = {.networkTest = test};
    assert(IHS_ClientStreamingRequest(client, &hosts[host], &r));
    drain(80); /* Initial production timer delay is 25ms. */
}
static void response(unsigned result, unsigned id, unsigned host) {
    CMsgRemoteClientBroadcastHeader h = CMSG_REMOTE_CLIENT_BROADCAST_HEADER__INIT;
    h.has_client_id = h.has_msg_type = true;
    h.client_id = hosts[host].clientId;
    h.msg_type = k_ERemoteDeviceStreamingResponse;
    CMsgRemoteDeviceStreamingResponse r = CMSG_REMOTE_DEVICE_STREAMING_RESPONSE__INIT;
    r.request_id = id;
    r.result = result;
    uint8_t key[32], secret[32], cipher[128];
    size_t capacity = sizeof(cipher);
    if (!result) {
        for (unsigned i = 0; i < 32; ++i) {
            key[i] = i;
            secret[i] = 32 + i;
        }
        assert(!IHS_CryptoSymmetricEncrypt(key, 32, secret, 32, cipher, &capacity));
        r.has_port = r.has_encrypted_session_key = true;
        r.port = 61024;
        r.encrypted_session_key = (ProtobufCBinaryData){capacity, cipher};
    }
    IHS_Buffer wire = IHS_BUFFER_INIT(1024, 0);
    IHS_BufferAppendMem(&wire, test_discovery_magic, 8);
    IHS_BufferAppendUInt32LE(&wire, cmsg_remote_client_broadcast_header__get_packed_size(&h));
    IHS_BufferAppendMessage(&wire, (const ProtobufCMessage *)&h);
    IHS_BufferAppendUInt32LE(&wire, cmsg_remote_device_streaming_response__get_packed_size(&r));
    IHS_BufferAppendMessage(&wire, (const ProtobufCMessage *)&r);
    client->base.callbacks.received(&client->base, &hosts[host].address, &wire);
    IHS_BufferClear(&wire, true);
    if (!result)
        IHS_ClientStreamingEstablished(client);
}
int main(int argc, char **argv) {
    assert(argc == 3);
    const char *path = argv[1];
    bool test = atoi(argv[2]);
    IHS_Init();
    uint8_t secret[32];
    for (unsigned i = 0; i < 32; ++i)
        secret[i] = 32 + i;
    IHS_ClientConfig config = {
        .deviceId = 1, .secretKey = secret, .deviceName = "transaction oracle"};
    client = IHS_ClientCreate(&config);
    assert(client);
    for (unsigned i = 0; i < 2; ++i) {
        sockets[i] = socket(AF_INET, SOCK_DGRAM, 0);
        assert(sockets[i] >= 0);
        struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
        assert(!bind(sockets[i], (void *)&a, sizeof(a)));
        socklen_t size = sizeof(a);
        assert(!getsockname(sockets[i], (void *)&a, &size));
        hosts[i].clientId = i ? 456 : 123;
        hosts[i].address.port = ntohs(a.sin_port);
        assert(IHS_IPAddressFromString(&hosts[i].address.ip, "127.0.0.1"));
    }
    bool ready = false;
    for (unsigned i = 0; i < 2000 && !ready; ++i) {
        IHS_BaseLock(&client->base);
        ready = client->base.socket != NULL;
        IHS_BaseUnlock(&client->base);
        if (!ready)
            poll(NULL, 0, 1);
    }
    assert(ready);
    start(test, 0);
    snapshot("requested");
    bool ticket = !strcmp(path, "cancel_ticket") || !strcmp(path, "replace_ticket") ||
                  !strcmp(path, "replace_ticket_host") || !strcmp(path, "failure_dialog");
    if (ticket) {
        response(0, 17, 0);
        snapshot("ticket");
        response(5, 17, 0);
        snapshot("matching_progress");
    }
    if (!strcmp(path, "duplicate_pending")) {
        start(test, 0);
        snapshot("duplicate");
    }
    if (!strncmp(path, "replace", 7)) {
        bool other = strstr(path, "_host") != NULL;
        start(false, other);
        snapshot("replaced");
        response(5, 17, 0);
        snapshot("old_progress");
        response(5, 19, other);
        snapshot("new_progress");
    }
    if (!strcmp(path, "failure_dialog")) {
        snapshot("failed_dialog");
        IHS_ClientStreamingCancel(client);
        snapshot("acknowledged");
    } else {
        IHS_ClientStreamingCancel(client);
        snapshot("cancelled");
        IHS_ClientStreamingCancel(client);
        snapshot("cancelled_again");
    }
    IHS_ClientStop(client);
    IHS_ClientThreadedJoin(client);
    IHS_ClientDestroy(client);
    close(sockets[0]);
    close(sockets[1]);
    IHS_Quit();
    return 0;
}
