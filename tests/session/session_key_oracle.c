/* Production session config, HMAC and control-frame crypto. No cipher doubles. */
#include "test_session.h"
#include "session/session_pri.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void hex(const uint8_t *data, size_t size) {
    for (size_t i = 0; i < size; ++i)
        printf("%02x", data[i]);
}
static size_t unhex(const char *raw, uint8_t *data) {
    if (!strcmp(raw, "-"))
        return 0;
    size_t size = strlen(raw) / 2;
    assert(size * 2 == strlen(raw) && size <= 1024);
    for (size_t i = 0; i < size; ++i) {
        unsigned value;
        assert(sscanf(raw + 2 * i, "%2x", &value) == 1);
        data[i] = value;
    }
    return size;
}
int main(int argc, char **argv) {
    assert(argc == 2);
    size_t length = strtoul(argv[1], NULL, 10);
    assert(length <= 32);
    IHS_Init();
    IHS_Session *s = IHS_TestSessionCreate();
    assert(s);
    s->info.sessionKeyLen = length;
    for (size_t i = 0; i < length; ++i)
        s->info.sessionKey[i] = i;
    bool configured = IHS_SessionConfigureNetworkTest(s, IHS_NetworkTestFixed, 20000);
    printf("{\"configured\":%s,\"hmac\":\"", configured ? "true" : "false");
    if (configured) {
        uint8_t hmac[32];
        size_t count = sizeof(hmac);
        static const uint8_t plain[] = "Steam In-Home Streaming";
        assert(!IHS_SessionFrameHMACSHA256(s, plain, sizeof(plain) - 1, hmac, &count));
        hex(hmac, count);
    }
    puts("\"}");
    char op, raw[2050];
    uint64_t sequence;
    while (configured && scanf(" %c %" SCNu64 " %2049s", &op, &sequence, raw) == 3) {
        uint8_t input[1040], output[1040];
        size_t size = unhex(raw, input);
        memset(output, 165, sizeof(output));
        bool ok;
        size_t out_count;
        if (op == 'E') {
            out_count = 1024;
            ok = IHS_SessionFrameEncrypt(s, input, size, output, &out_count, sequence) == 0;
        } else {
            assert(op == 'D');
            IHS_Buffer in = IHS_BUFFER_INIT(128, 0), out = IHS_BUFFER_INIT(128, 0);
            if (size)
                IHS_BufferAppendMem(&in, input, size);
            ok = IHS_SessionFrameDecrypt(s, &in, &out, sequence, NULL) == IHS_SessionFrameDecryptOK;
            out_count = out.size;
            assert(out_count <= 1024);
            if (ok)
                memcpy(output, IHS_BufferPointer(&out), out_count);
            IHS_BufferClear(&out, true);
            IHS_BufferClear(&in, true);
        }
        for (size_t i = 1024; i < sizeof(output); ++i)
            assert(output[i] == 165);
        printf("{\"ok\":%s,\"output\":", ok ? "true" : "false");
        if (ok) {
            putchar('"');
            hex(output, out_count);
            putchar('"');
        } else {
            printf("null");
        }
        puts("}");
    }
    IHS_SessionDestroy(s);
    IHS_Quit();
    return 0;
}
