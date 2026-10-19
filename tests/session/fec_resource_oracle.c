/* FEC requested/live allocation accounting, not malloc overhead/RSS/GPU peak.
 * Only actual production calloc/free calls are tracked. Input/printing/hash
 * scratch is excluded. Failed allocations are explicit test adapters. */
#include "crypto.h"
#include "session/fec.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct allocation {
    void *pointer;
    size_t bytes;
} allocation;
static allocation owned[32];
static bool tracking;
static unsigned fail_at, requests;
static size_t live, peak, requested;
void *__real_calloc(size_t, size_t);
void *__wrap_calloc(size_t n, size_t width) {
    bool fail = tracking && fail_at && !--fail_at;
    void *p = fail ? NULL : __real_calloc(n, width);
    if (tracking) {
        assert(!width || n <= SIZE_MAX / width);
        size_t bytes = n * width;
        ++requests;
        requested += bytes;
        if (p) {
            unsigned i = 0;
            while (i < 32 && owned[i].pointer)
                ++i;
            assert(i < 32);
            owned[i] = (allocation){p, bytes};
            live += bytes;
            if (peak < live)
                peak = live;
        }
    }
    return p;
}
void __real_free(void *);
void __wrap_free(void *p) {
    for (unsigned i = 0; p && i < 32; ++i)
        if (owned[i].pointer == p) {
            live -= owned[i].bytes;
            owned[i] = (allocation){0};
            break;
        }
    __real_free(p);
}
static IHS_FECReceiver *create(void) {
    tracking = true;
    IHS_FECReceiver *r = IHS_FECReceiverCreate();
    tracking = false;
    assert(r);
    return r;
}
static bool add(IHS_FECReceiver *r, IHS_SessionPacket *p, IHS_SessionFrame *out) {
    tracking = true;
    bool ready = IHS_FECReceiverAdd(r, p, out);
    tracking = false;
    return ready;
}
static void allocation_domain(void) {
    const size_t old_length = 2u * 1024u * 1024u;
    size_t old_data_max = 0, old_work_max = 0, new_work_max = 0;
    for (size_t s = 1; s <= UINT16_MAX; ++s) {
        size_t old_k = (old_length + s - 1) / s;
        size_t bytes = old_k * s;
        if (old_data_max < bytes)
            old_data_max = bytes;
        /* For parity>0, k+m<=255 and missing<=min(k,m). At fixed S the
         * maximum is total255 with missing=min(127,kmax). Include presence,
         * matrix2*missing^2 and RHS; fixed receiver/allocator/stack excluded. */
        size_t old_missing = old_k < 127 ? old_k : 127;
        size_t new_k = IHS_FEC_MAX_DATA_BYTES / s;
        size_t new_missing = new_k < 127 ? new_k : 127;
        size_t old_work = 255 * s + old_missing * s + 2 * old_missing * old_missing + 255;
        size_t new_work = 255 * s + new_missing * s + 2 * new_missing * new_missing + 255;
        if (old_work_max < old_work)
            old_work_max = old_work;
        if (new_work_max < new_work)
            new_work_max = new_work;
        assert(bytes <= IHS_FEC_MAX_DATA_BYTES);
    }
    assert(old_data_max == 2162655 && old_data_max == IHS_FEC_MAX_DATA_BYTES);
    assert(old_work_max == 18876513 && new_work_max == old_work_max);
    /* Real admission at the data-storage boundary, without materializing
     * every metadata combination or claiming an OS/device memory peak. */
    for (unsigned test = 0; test < 3; ++test) {
        IHS_FECReceiver *r = create();
        size_t base = live;
        unsigned char *bytes = calloc(65543, 1);
        assert(bytes);
        uint32_t length = IHS_FEC_MAX_DATA_BYTES + (test == 1);
        unsigned shard = test == 2 ? 65534 : 65535;
        bytes[0] = length;
        bytes[1] = length >> 8;
        bytes[2] = length >> 16;
        bytes[4] = shard;
        bytes[5] = shard >> 8;
        IHS_SessionPacket p = {.header = {.type = IHS_SessionPacketTypeUnreliableFEC},
                               .body = IHS_BUFFER_WRAP(bytes, shard + 8)};
        IHS_SessionFrame out = {0};
        assert(!add(r, &p, &out) && IHS_FECReceiverLastError(r) == IHS_FECReceiveOK);
        assert(test ? live == base : live == base + IHS_FEC_MAX_DATA_BYTES + 33);
        free(bytes);
        IHS_FECReceiverDestroy(r);
        assert(!live);
    }
}
static void self_test(void) {
    allocation_domain();
    /* Valid k1/m1 parity coefficient is inverse(1)=1. Sending only index1
     * therefore needs real RS and recovers 'z', without a test encoder model. */
    for (unsigned stage = 1; stage <= 4; ++stage) {
        IHS_FECReceiver *r = create();
        unsigned char bytes[40] = {1, 0, 0, 0, 32, 0, 1, 0, 'z'};
        IHS_SessionPacket p = {
            .header = {.type = IHS_SessionPacketTypeUnreliableFEC, .fragmentId = 1},
            .body = IHS_BUFFER_WRAP(bytes, sizeof(bytes))};
        IHS_SessionFrame out = {.header = {.packetId = 12345}}, before = out;
        fail_at = stage;
        assert(!add(r, &p, &out) && !fail_at);
        assert(IHS_FECReceiverLastError(r) == IHS_FECReceiveNoMemory);
        assert(!memcmp(&out, &before, sizeof(out)));
        /* A malformed call resets per-call status and must not allocate or
         * report a stale OOM. The session caller already closes on the error. */
        p.header.type = IHS_SessionPacketTypeUnreliable;
        assert(!add(r, &p, &out) && IHS_FECReceiverLastError(r) == IHS_FECReceiveOK);
        p.header.type = IHS_SessionPacketTypeUnreliableFEC;
        p.header.fragmentId = 0; /* new systematic data, not a duplicate retry */
        assert(add(r, &p, &out) && IHS_FECReceiverLastError(r) == IHS_FECReceiveOK);
        assert(out.body.size == 1 && *IHS_BufferPointer(&out.body) == 'z');
        IHS_BufferClear(&out.body, true);
        assert(!add(r, &p, &out) && IHS_FECReceiverLastError(r) == IHS_FECReceiveOK);
        IHS_FECReceiverDestroy(r);
        assert(!live);
    }
    /* Incomplete valid group is a normal false, without a resource error. */
    IHS_FECReceiver *r = create();
    unsigned char bytes[40] = {33, 0, 0, 0, 32};
    IHS_SessionPacket p = {.header = {.type = IHS_SessionPacketTypeUnreliableFEC},
                           .body = IHS_BUFFER_WRAP(bytes, sizeof(bytes))};
    IHS_SessionFrame out = {0};
    assert(!add(r, &p, &out) && IHS_FECReceiverLastError(r) == IHS_FECReceiveOK);
    IHS_FECReceiverDestroy(r);
    assert(!live);
    puts("FEC allocation failures: shards/presence/matrix/RHS, error reset and ownership passed");
}
static void observe(void) {
    IHS_FECReceiver *r = create();
    size_t receiver_bytes = live;
    unsigned delivered = 0, packets = 0, resource_errors = 0;
    size_t length = 0, capacity = 0;
    char hash_text[65] = "", *line = NULL;
    size_t line_capacity = 0;
    ssize_t chars;
    while ((chars = getline(&line, &line_capacity, stdin)) >= 0) {
        while (chars && (line[chars - 1] == '\n' || line[chars - 1] == '\r'))
            --chars;
        assert(chars > 0 && !(chars & 1) && chars <= 131200);
        unsigned char *wire = malloc((size_t)chars / 2);
        assert(wire);
        for (ssize_t i = 0; i < chars / 2; ++i) {
            unsigned byte;
            assert(sscanf(line + i * 2, "%2x", &byte) == 1);
            wire[i] = (unsigned char)byte;
        }
        IHS_Buffer input = IHS_BUFFER_WRAP(wire, (size_t)chars / 2);
        IHS_SessionPacket p = {0};
        IHS_SessionFrame out = {0};
        assert(IHS_SessionPacketParse(&p, &input) == IHS_SessionPacketResultOK);
        ++packets;
        bool ready = add(r, &p, &out);
        resource_errors += IHS_FECReceiverLastError(r) != IHS_FECReceiveOK;
        if (ready) {
            ++delivered;
            length = out.body.size;
            capacity = out.body.capacity;
            unsigned char hash[32];
            assert(!IHS_CryptoSHA256(IHS_BufferPointer(&out.body), out.body.size, hash));
            for (unsigned i = 0; i < 32; ++i)
                sprintf(hash_text + i * 2, "%02x", hash[i]);
        }
        IHS_BufferClear(&out.body, true);
        IHS_SessionPacketClear(&p, true);
        IHS_BufferClear(&input, true);
    }
    free(line);
    size_t retained = live;
    IHS_FECReceiverDestroy(r);
    assert(!live);
    printf("{\"packets\":%u,\"delivered\":%u,\"bytes\":%zu,\"capacity\":%zu,"
           "\"sha256\":\"%s\",\"receiver_bytes\":%zu,\"allocation_requests\":%u,"
           "\"requested_bytes\":%zu,\"owned_peak\":%zu,\"retained_before_destroy\":%zu,"
           "\"resource_errors\":%u,\"live_after_destroy\":%zu}\n",
           packets, delivered, length, capacity, hash_text, receiver_bytes, requests, requested,
           peak, retained, resource_errors, live);
}
int main(int argc, char **argv) {
    assert(argc == 2);
    if (!strcmp(argv[1], "--self-test"))
        self_test();
    else {
        assert(!strcmp(argv[1], "--observe"));
        observe();
    }
    return 0;
}
