/* Native receiver driver for fixed official packet bytes. No encoder model. */
#include "session/fec.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    IHS_FECReceiver *r = IHS_FECReceiverCreate();
    assert(r);
    char *line = NULL;
    size_t capacity = 0;
    ssize_t length;
    while ((length = getline(&line, &capacity, stdin)) >= 0) {
        if (line[0] == 'R') {
            IHS_FECReceiverDestroy(r);
            r = IHS_FECReceiverCreate();
            assert(r);
            puts("reset");
            continue;
        }
        while (length && (line[length - 1] == '\n' || line[length - 1] == '\r'))
            line[--length] = 0;
        assert(length > 0 && !(length & 1) && length <= 131200);
        uint8_t *wire = malloc((size_t)length / 2);
        assert(wire);
        for (ssize_t i = 0; i < length / 2; ++i) {
            unsigned byte;
            assert(sscanf(line + i * 2, "%2x", &byte) == 1);
            wire[i] = (uint8_t)byte;
        }
        IHS_Buffer input = IHS_BUFFER_WRAP(wire, (size_t)length / 2);
        IHS_SessionPacket packet;
        IHS_SessionFrame frame;
        int parsed = IHS_SessionPacketParse(&packet, &input);
        if (parsed != IHS_SessionPacketResultOK) {
            puts("invalid");
        } else if (IHS_FECReceiverAdd(r, &packet, &frame)) {
            printf("%u %u %u %u ", frame.header.type, frame.header.packetId,
                   frame.header.sendTimestamp, frame.header.fragmentId & 65535);
            for (size_t i = 0; i < frame.body.size; ++i)
                printf("%02x", IHS_BufferPointer(&frame.body)[i]);
            putchar('\n');
            IHS_BufferClear(&frame.body, true);
        } else {
            puts("-");
        }
        IHS_SessionPacketClear(&packet, true);
        IHS_BufferClear(&input, true);
    }
    free(line);
    IHS_FECReceiverDestroy(r);
    return 0;
}
