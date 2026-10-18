/* Real UDP/auth/cipher/owner and teardown; peer is controlled, no Steam ticket
 * acquisition or device/decoder is claimed. Source body/HMAC constants §119. */
#include "crc32c.h"
#include "ihs_buffer.h"
#include "ihs_buffer_ext.h"
#include "ihslib.h"
#include "network_test_fec.h"
#include "session/session_pri.h"
#include <arpa/inet.h>
#include <assert.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
static atomic_bool finalized;
static void finished(IHS_Session *s, void *p) {
    (void)s;
    (void)p;
    atomic_store(&finalized, true);
}
static void unexpected_connected(IHS_Session *s, void *p) {
    (void)s;
    (void)p;
    assert(false);
}
static uint64_t millis(void) {
    struct timespec t;
    assert(!clock_gettime(CLOCK_MONOTONIC, &t));
    return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static void send_peer(int fd, const struct sockaddr_in *to, uint8_t local, unsigned type,
                      unsigned ch, unsigned id, const uint8_t *body, size_t count, bool crc) {
    uint8_t wire[512] = {type, 0, 7, local, ch};
    wire[7] = id;
    wire[8] = id >> 8;
    uint32_t sent = 1000000;
    memcpy(wire + 9, &sent, 4);
    memcpy(wire + 13, body, count);
    count += 13;
    if (crc) {
        wire[0] |= 128;
        uint32_t sum = IHS_CRC32C(wire, count);
        memcpy(wire + count, &sum, 4);
        count += 4;
    }
    assert(sendto(fd, wire, count, 0, (const struct sockaddr *)to, sizeof(*to)) == (ssize_t)count);
}
static void encrypted(IHS_Session *s, int fd, const struct sockaddr_in *to, uint8_t local,
                      unsigned id, int kbps, unsigned seq, bool crc) {
    uint8_t plain[16] = {8};
    size_t n = 1;
    unsigned v = kbps;
    while (v > 127) {
        plain[n++] = (uint8_t)v | 128;
        v >>= 7;
    }
    plain[n++] = v;
    uint8_t body[256] = {94};
    size_t size = sizeof(body) - 1;
    assert(!IHS_SessionFrameEncrypt(s, plain, n, body + 1, &size, seq));
    send_peer(fd, to, local, 5, 1, id, body, size + 1, crc);
}
static void one(bool crc, IHS_NetworkTestMode mode, bool fec) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    assert(fd >= 0);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(!bind(fd, (void *)&addr, sizeof(addr)));
    socklen_t len = sizeof(addr);
    assert(!getsockname(fd, (void *)&addr, &len));
    IHS_SessionInfo info = {
        .address.port = ntohs(addr.sin_port), .sessionKeyLen = 32, .steamId = 123456};
    assert(IHS_IPAddressFromString(&info.address.ip, "127.0.0.1"));
    for (unsigned i = 0; i < 32; ++i)
        info.sessionKey[i] = i;
    const uint8_t secret[32] = {0};
    IHS_ClientConfig client = {
        .deviceId = 1, .secretKey = secret, .deviceName = "network-test peer"};
    IHS_Session *s = IHS_SessionCreate(&client, &info);
    assert(s);
    assert(IHS_SessionConfigureNetworkTest(s, mode, 20000));
    const IHS_StreamSessionCallbacks cb = {.connected = unexpected_connected,
                                           .finalized = finished};
    IHS_SessionSetSessionCallbacks(s, &cb, NULL);
    atomic_store(&finalized, false);
    assert(IHS_SessionConnect(s));
    bool connected = false, handshake = false, auth = false, start = false, sample = false,
         target2 = false, cancel = false;
    unsigned keepalive = 0;
    uint64_t sequence = 0, until = millis() + 5000;
    uint8_t local = 0;
    struct sockaddr_in from = {0};
    while (millis() < until && !atomic_load(&finalized)) {
        IHS_NetworkTestResult snapshot;
        IHS_SessionGetNetworkTestResult(s, &snapshot);
        if (mode == IHS_NetworkTestBandwidth && snapshot.samples && !target2) {
            encrypted(s, fd, &from, local, 3, 99999, 1, crc);
            target2 = true;
        }
        if (mode == IHS_NetworkTestBandwidth && snapshot.previousTargetKbps == 100000 &&
            snapshot.targetKbps == 99999 && !cancel) {
            IHS_SessionDisconnect(s);
            cancel = true;
        }
        struct pollfd p = {.fd = fd, .events = POLLIN};
        int ready = poll(&p, 1, 5);
        assert(ready >= 0);
        if (!ready)
            continue;
        uint8_t wire[4096];
        socklen_t flen = sizeof(from);
        ssize_t size = recvfrom(fd, wire, sizeof(wire), 0, (void *)&from, &flen);
        assert(size >= 13);
        local = wire[2];
        unsigned type = wire[0] & 127;
        if (wire[0] & 128) {
            assert(size >= 17);
            uint32_t sum;
            memcpy(&sum, wire + size - 4, 4);
            assert(IHS_CRC32C(wire, size - 4) == sum);
            size -= 4;
        }
        if (type == 1) {
            if (!connected) {
                send_peer(fd, &from, local, 2, 0, 0, wire + 9, 4, crc);
                connected = true;
            }
            continue;
        }
        if (type != 5 || wire[4] != 1)
            continue;
        uint8_t tag = wire[13];
        if (tag == 6) {
            if (!handshake) {
                assert(size == 18 && !memcmp(wire + 13, "\x06\x0a\x02\x10\x01", 5));
                const uint8_t body[] = {7, 10, 3, 8, 0xc0, 0x0b};
                send_peer(fd, &from, local, 5, 1, 0, body, sizeof(body), crc);
                handshake = true;
            }
            continue;
        }
        if (tag == 1) {
            if (!auth) {
                static const uint8_t expected[] = {
                    1,    10,   32,   0x8f, 0xf3, 0xa6, 0xe9, 0xdf, 0x0b, 0xb5, 0xaa, 0x37, 0x68,
                    0x46, 0xc0, 0x1b, 0xfd, 0x2c, 0x0c, 0x8a, 0xf6, 0x99, 0x4e, 0x22, 0x5c, 0x0a,
                    0xac, 0x69, 0x45, 0x94, 0x9b, 0xb4, 0x2b, 0xb6, 0x71, 16,   1};
                assert((size_t)(size - 13) == sizeof(expected) &&
                       !memcmp(wire + 13, expected, sizeof(expected)));
                const uint8_t body[] = {2, 16, 1};
                send_peer(fd, &from, local, 5, 1, 1, body, sizeof(body), crc);
                auth = true;
            }
            continue;
        }
        assert(tag == 9 || tag == 8);
        IHS_Buffer cipher = IHS_BUFFER_WRAP(wire + 14, size - 14), plain = IHS_BUFFER_INIT(128, 0);
        uint64_t actual;
        IHS_SessionFrameDecryptResult result =
            IHS_SessionFrameDecrypt(s, &cipher, &plain, sequence, &actual);
        assert(result == IHS_SessionFrameDecryptOK || result == IHS_SessionFrameDecryptOldSequence);
        if (result == IHS_SessionFrameDecryptOldSequence) {
            IHS_BufferClear(&plain, true);
            continue;
        }
        ++sequence;
        if (tag == 9) {
            assert(!plain.size);
            ++keepalive;
        } else if (!start) {
            static const uint8_t fixed[] = {8, 0xd8, 4, 16, 60, 24, 0xa0, 0x9c, 1, 32, 0, 40, 0};
            uint8_t expected[sizeof(fixed)];
            memcpy(expected, fixed, sizeof(fixed));
            expected[sizeof(fixed) - 1] = mode == IHS_NetworkTestBandwidth;
            assert(keepalive == 3 && plain.size == sizeof(expected) &&
                   !memcmp(IHS_BufferPointer(&plain), expected, sizeof(expected)));
            start = true;
            if (mode == IHS_NetworkTestBandwidth)
                encrypted(s, fd, &from, local, 2, 100000, 0, crc);
            uint8_t body[13] = {1};
            unsigned fid = mode == IHS_NetworkTestFixed ? 599 : 0;
            body[1] = fid;
            body[2] = fid >> 8;
            if (fec) {
                for (unsigned f = 0; f < 4; ++f) {
                    uint8_t raw[128];
                    size_t count = network_test_fec_sizes[f];
                    memcpy(raw, network_test_fec[f], count);
                    raw[2] = 7;
                    raw[3] = local;
                    uint32_t sent = 1000000;
                    memcpy(raw + 9, &sent, 4);
                    if (crc) {
                        raw[0] |= 128;
                        uint32_t sum = IHS_CRC32C(raw, count);
                        memcpy(raw + count, &sum, 4);
                        count += 4;
                    }
                    assert(sendto(fd, raw, count, 0, (void *)&from, sizeof(from)) ==
                           (ssize_t)count);
                }
            } else
                send_peer(fd, &from, local, 3, 3, 0, body, sizeof(body), crc);
            sample = true;
        }
        IHS_BufferClear(&plain, true);
    }
    bool naturally_finished = atomic_load(&finalized);
    if (!naturally_finished)
        IHS_SessionDisconnect(s);
    IHS_SessionThreadedJoin(s);
    assert(naturally_finished && connected && handshake && auth && start && sample);
    IHS_NetworkTestResult result;
    IHS_SessionGetNetworkTestResult(s, &result);
    assert(result.valid && result.samples == 1);
    assert(result.status ==
           (mode == IHS_NetworkTestFixed ? IHS_NetworkTestComplete : IHS_NetworkTestCancelled));
    assert(result.frameSpan == (mode == IHS_NetworkTestFixed ? 600u : 1u));
    assert(s->numChannels == 3 && !s->callbacks.video && !s->callbacks.audio);
    IHS_NetworkTestSample full;
    assert(IHS_SessionReadNetworkTestSamples(s, 0, &full, 1) == 1);
    if (mode == IHS_NetworkTestBandwidth)
        assert(full.targetKbps == 100000);
    IHS_SessionDestroy(s);
    close(fd);
}
int main(void) {
    IHS_Init();
    for (unsigned crc = 0; crc < 2; ++crc) {
        one(crc, IHS_NetworkTestFixed, false);
        one(crc, IHS_NetworkTestFixed, true);
        one(crc, IHS_NetworkTestBandwidth, false);
    }
    IHS_Quit();
    puts("Network-test real UDP: CRC x ordinary/FEC fixed-complete/bandwidth-cancel, test "
         "handshake/auth/crypto/request8, target94/full samples, no decoder, joined teardown "
         "passed");
    return 0;
}
