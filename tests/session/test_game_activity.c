/* Verifies the optional launch ID on the wire and borrowed host activity callback. */
#include "ihs_buffer.h"
#include "ihs_buffer_ext.h"
#include "ihslib/client.h"
#include "protobuf/discovery.pb-c.h"
#include "protobuf/remoteplay.pb-c.h"
#include "session/channels/ch_control.h"
#include "session/channels/channel.h"
#include "test_session.h"
#include <arpa/inet.h>
#include <assert.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
static unsigned calls;
static void activity(IHS_Session *s, uint64_t id, const char *name, void *ctx) {
    (void)s;
    (void)ctx;
    assert(id == UINT64_C(0x123456789abcdef));
    assert(!strcmp(name, "Host game"));
    calls++;
}
static uint32_t le32(const uint8_t *p) {
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
int main(void) {
    IHS_Init();
    IHS_Session *s = IHS_TestSessionCreate();
    assert(s);
    IHS_StreamInputCallbacks cb = {.activity = activity};
    IHS_SessionSetInputCallbacks(s, &cb, NULL);
    IHS_SessionChannel channel = {.session = s};
    CSetActivityMsg msg = CSET_ACTIVITY_MSG__INIT;
    msg.has_gameid = true;
    msg.gameid = UINT64_C(0x123456789abcdef);
    msg.game_name = "Host game";
    for (int i = 0; i < 3; ++i) {
        if (i == 1)
            msg.gameid = 0;
        if (i == 2) {
            msg.gameid = 123;
            msg.game_name = NULL;
        }
        IHS_Buffer buf;
        IHS_BufferInit(&buf, 128, 128);
        IHS_BufferAppendMessage(&buf, (ProtobufCMessage *)&msg);
        IHS_SessionChannelControlOnMessageReceived(&channel, k_EStreamControlSetActivity, &buf,
                                                   NULL);
        IHS_BufferClear(&buf, true);
    }
    assert(calls == 1);
    IHS_SessionDestroy(s);
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    assert(fd >= 0);
    struct sockaddr_in addr = {.sin_family = AF_INET,
                               .sin_addr = {.s_addr = htonl(INADDR_LOOPBACK)}};
    assert(!bind(fd, (struct sockaddr *)&addr, sizeof(addr)));
    socklen_t len = sizeof(addr);
    assert(!getsockname(fd, (struct sockaddr *)&addr, &len));
    struct timeval timeout = {.tv_sec = 2};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    IHS_Client *client = IHS_ClientCreate(&clientConfig);
    assert(client);
    IHS_HostInfo host = {.clientId = 23};
    assert(IHS_IPAddressFromString(&host.address.ip, "127.0.0.1"));
    host.address.port = ntohs(addr.sin_port);
    for (int i = 0; i < 2; ++i) {
        if (i) {
            IHS_ClientStop(client);
            IHS_ClientThreadedJoin(client);
            IHS_ClientDestroy(client);
            client = IHS_ClientCreate(&clientConfig);
            assert(client);
        }
        IHS_StreamingRequest req = {.gameId = i ? 0 : UINT64_C(0x123456789abcdef),
                                    .streamingInterface = IHS_StreamInterfaceBigPicture};
        assert(IHS_ClientStreamingRequest(client, &host, &req));
        uint8_t bytes[4096];
        ssize_t n = recv(fd, bytes, sizeof(bytes), 0);
        assert(n > 16);
        uint32_t header = le32(bytes + 8);
        assert(header + 16 < (uint32_t)n);
        uint32_t size = le32(bytes + 12 + header);
        assert(header + 16 + size == (uint32_t)n);
        CMsgRemoteDeviceStreamingRequest *wire =
            cmsg_remote_device_streaming_request__unpack(NULL, size, bytes + 16 + header);
        assert(wire);
        assert(wire->has_gameid == !i);
        if (!i)
            assert(wire->gameid == req.gameId);
        assert(wire->stream_interface == k_EStreamInterfaceBigPicture);
        cmsg_remote_device_streaming_request__free_unpacked(wire, NULL);
    }
    IHS_ClientStop(client);
    IHS_ClientThreadedJoin(client);
    IHS_ClientDestroy(client);
    close(fd);
    IHS_Quit();
    return 0;
}
