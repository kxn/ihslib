/* Controlled discovery peer envelope for Linux tests; not a Steam host. */
#pragma once
#include "ihs_buffer.h"
#include "ihs_buffer_ext.h"
#include "protobuf/discovery.pb-c.h"
#include <assert.h>
#include <netinet/in.h>
#include <sys/socket.h>
static const uint8_t test_discovery_magic[8] = {255, 255, 255, 255, 0x21, 0x4c, 0x5f, 0xa0};
static uint32_t test_discovery_u32(const uint8_t *p) {
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void test_discovery_send(int fd, const struct sockaddr_in *to, const uint8_t *body,
                                size_t count) {
    CMsgRemoteClientBroadcastHeader h = CMSG_REMOTE_CLIENT_BROADCAST_HEADER__INIT;
    h.has_client_id = h.has_msg_type = true;
    h.client_id = 123;
    h.msg_type = k_ERemoteDeviceStreamingResponse;
    IHS_Buffer wire = IHS_BUFFER_INIT(1024, 0);
    IHS_BufferAppendMem(&wire, test_discovery_magic, 8);
    size_t hs = cmsg_remote_client_broadcast_header__get_packed_size(&h);
    IHS_BufferAppendUInt32LE(&wire, hs);
    IHS_BufferAppendMessage(&wire, (const ProtobufCMessage *)&h);
    IHS_BufferAppendUInt32LE(&wire, count);
    IHS_BufferAppendMem(&wire, body, count);
    assert(sendto(fd, IHS_BufferPointer(&wire), wire.size, 0, (const struct sockaddr *)to,
                  sizeof(*to)) == (ssize_t)wire.size);
    IHS_BufferClear(&wire, true);
}
