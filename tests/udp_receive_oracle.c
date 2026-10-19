/* Actual Base worker -> real POSIX UDP receive -> CRC parse/window.
 * Only the peer datagram is a fixture. No Steam/auth/codec/device claim.
 * A protocol-owned receiveCycle selects the same Base buffer as Session. */
#include "base.h"
#include "crypto.h"
#include "ihs_buffer.h"
#include "ihs_udp.h"
#include "session/packet.h"
#include "session/window.h"
#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int peer;
static struct sockaddr_in destination;
static unsigned char *wire;
static size_t wire_bytes;
static size_t received_bytes, receive_capacity, delivered_bytes;
static bool delivered;
static char wire_hash[65], body_hash[65];
static bool script_select, clear_sets;
static int select_result;
static unsigned select_calls;
int __real_select(int, fd_set *, fd_set *, fd_set *, struct timeval *);
int __wrap_select(int n, fd_set *r, fd_set *w, fd_set *e, struct timeval *timeout) {
    if (!script_select)
        return __real_select(n, r, w, e, timeout);
    ++select_calls;
    assert(r && !w && !e && timeout->tv_sec == 0 && timeout->tv_usec == 2000);
    if (clear_sets)
        FD_ZERO(r);
    errno = EIO;
    return select_result;
}
static void hash(const unsigned char *bytes, size_t n, char *text) {
    unsigned char digest[32];
    assert(!IHS_CryptoSHA256(bytes, n, digest));
    for (unsigned i = 0; i < 32; ++i)
        sprintf(text + 2 * i, "%02x", digest[i]);
}
static void initialized(IHS_Base *base, void *context) {
    (void)context;
    assert(IHS_UDPSocketSetBlocking(base->socket, false));
    unsigned char byte = 0;
    IHS_UDPPacket tx = {.buffer = IHS_BUFFER_WRAP(&byte, 1)};
    assert(IHS_IPAddressFromString(&tx.address.ip, "127.0.0.1"));
    tx.address.port = ntohs(destination.sin_port);
    assert(IHS_UDPSocketSend(base->socket, &tx));
    struct sockaddr_in reply;
    socklen_t reply_bytes = sizeof(reply);
    assert(recvfrom(peer, &byte, 1, 0, (void *)&reply, &reply_bytes) == 1);
    assert(sendto(peer, wire, wire_bytes, 0, (void *)&reply, reply_bytes) == (ssize_t)wire_bytes);
}
static bool receive_cycle(IHS_Base *base, void *context, IHS_UDPPacket *rx) {
    (void)context;
    assert(IHS_UDPSocketWaitReadable(base->socket, 1000) == 1);
    assert(IHS_UDPSocketReceive(base->socket, rx) == 1);
    receive_capacity = rx->buffer.capacity;
    received_bytes = rx->buffer.size;
    hash(IHS_BufferPointer(&rx->buffer), rx->buffer.size, wire_hash);
    IHS_SessionPacket packet = {0};
    IHS_SessionPacketReturn result = IHS_SessionPacketParse(&packet, &rx->buffer);
    if (result == IHS_SessionPacketResultOK) {
        IHS_SessionPacketsWindow *window = IHS_SessionPacketsWindowCreateMedia(32);
        assert(window && IHS_SessionPacketsWindowAdd(window, &packet));
        IHS_SessionFrame frame = {.body = IHS_BUFFER_INIT(2048, 0)};
        delivered = IHS_SessionPacketsWindowPoll(window, &frame);
        if (delivered) {
            delivered_bytes = frame.body.size;
            hash(IHS_BufferPointer(&frame.body), frame.body.size, body_hash);
        }
        IHS_BufferClear(&frame.body, true);
        IHS_SessionPacketsWindowDestroy(window);
        IHS_SessionPacketClear(&packet, true);
    }
    return false;
}
int main(int argc, char **argv) {
    if (argc == 4 && !strcmp(argv[1], "--wait")) {
        IHS_Init();
        script_select = true;
        select_result = atoi(argv[2]);
        clear_sets = atoi(argv[3]);
        IHS_UDPSocket *socket = IHS_UDPSocketOpen(false);
        assert(socket);
        int ready = IHS_UDPSocketWaitReadable(socket, 2);
        IHS_UDPSocketClose(socket);
        IHS_Quit();
        printf("{\"readable\":%d,\"select_calls\":%u}\n", ready, select_calls);
        return 0;
    }
    assert(argc == 2);
    alarm(10);
    size_t chars = strlen(argv[1]);
    assert(chars && !(chars & 1) && chars <= 65507 * 2);
    wire_bytes = chars / 2;
    wire = malloc(wire_bytes);
    assert(wire);
    for (size_t n = 0; n < wire_bytes; ++n) {
        unsigned byte;
        assert(sscanf(argv[1] + 2 * n, "%2x", &byte) == 1);
        wire[n] = byte;
    }
    IHS_Init();
    peer = socket(AF_INET, SOCK_DGRAM, 0);
    assert(peer >= 0);
    destination =
        (struct sockaddr_in){.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(!bind(peer, (void *)&destination, sizeof(destination)));
    socklen_t size = sizeof(destination);
    assert(!getsockname(peer, (void *)&destination, &size));
    struct timeval timeout = {.tv_sec = 1};
    assert(!setsockopt(peer, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)));
    IHS_Base base;
    const uint8_t secret[32] = {0};
    const IHS_ClientConfig client = {
        .deviceId = 1, .secretKey = secret, .deviceName = "UDP fixture"};
    IHS_BaseInit(&base, &client, NULL, false);
    const IHS_BaseRunCallbacks callbacks = {.initialized = initialized,
                                            .receiveCycle = receive_cycle};
    IHS_BaseSetRunCallbacks(&base, &callbacks, NULL);
    assert(IHS_BaseStartWorker(&base, "UDPFixture"));
    IHS_BaseWaitWorker(&base);
    assert(!base.worker && !base.socket);
    IHS_BaseDestroy(&base);
    close(peer);
    IHS_Quit();
    free(wire);
    printf("{\"wire_bytes\":%zu,\"received_bytes\":%zu,\"capacity\":%zu,\"received_sha256\":\"%s\","
           "\"delivered\":%s,\"body_bytes\":%zu,\"body_sha256\":\"%s\",\"joined\":true}\n",
           wire_bytes, received_bytes, receive_capacity, wire_hash, delivered ? "true" : "false",
           delivered_bytes, body_hash);
    return 0;
}
