/* Final syscall is controlled: a full send, EAGAIN and EINTR each get one
 * nonblocking attempt; the socket's receive blocking mode stays intact. */
#include "ihs_buffer.h"
#include "ihs_udp.h"
#include "ihslib/common.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
static unsigned calls;
static int error;
static bool shortSend;
ssize_t __wrap_sendto(int fd, const void *data, size_t size, int flags, const struct sockaddr *addr,
                      socklen_t length) {
    ++calls;
    assert(flags & MSG_DONTWAIT);
    assert(!(fcntl(fd, F_GETFL) & O_NONBLOCK));
    assert(size == 3 && !memcmp(data, "udp", 3));
    assert(addr->sa_family == AF_INET && length == sizeof(struct sockaddr_in));
    errno = error;
    return error ? -1 : (ssize_t)size - (shortSend ? 1 : 0);
}
int main(void) {
    IHS_Init();
    IHS_UDPSocket *socket = IHS_UDPSocketOpen(false);
    IHS_UDPPacket packet = {.address = {.ip = {.family = IHS_IPAddressFamilyIPv4}, .port = 9}};
    uint8_t bytes[] = "udp";
    packet.buffer = (IHS_Buffer)IHS_BUFFER_WRAP(bytes, 3);
    const int errors[] = {0, EAGAIN, EINTR, 0};
    for (unsigned i = 0; i < 4; ++i) {
        error = errors[i];
        shortSend = i == 3;
        assert(IHS_UDPSocketSend(socket, &packet) == (error == 0 && !shortSend));
        if (error)
            assert(errno == error);
        assert(calls == i + 1);
    }
    IHS_UDPSocketClose(socket);
    IHS_Quit();
    return 0;
}
