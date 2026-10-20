/* Actual production consumer/tracker/reset observer and real crypto. CPU
 * variants are a Source-only selector; the port has one byte formatter. */
#include "test_session.h"
#include "session/session_pri.h"
#include <assert.h>
static IHS_Session *nalSession(void) {
    IHS_Session *s = IHS_TestSessionCreate();
    assert(s);
    for (unsigned n = 0; n < 16; ++n) s->info.sessionKey[n] = n;
    s->info.sessionKeyLen = 16;
    return s;
}
#define IHS_TestSessionCreate nalSession
#define main mode_vectors_main
#include "video_content_mode_oracle.c"
#undef main
#undef IHS_TestSessionCreate
static void guards(void) {
    IHS_Init();
    for (unsigned codec = 4; codec <= 5; ++codec) {
        IHS_SessionChannelVideo channel = {0};
        channel.config.codec = codec;
        channel.frame.buffer = (IHS_Buffer)IHS_BUFFER_INIT(8, 256);
        IHS_Buffer data = IHS_BUFFER_INIT(8, 256);
        IHS_VideoFrameHeader header = {.flags = VideoFrameFlagNeedEscape |
                                              VideoFrameFlagNeedStartSequence};
        assert(!AppendToFrameBuffer(&channel, &data, &header));
        assert(!channel.frame.buffer.size && !channel.frame.hasPacketBuffers);
        IHS_BufferAppendUInt8(&data, 0);
        header.flags = VideoFrameFlagNeedEscape;
        assert(!AppendToFrameBuffer(&channel, &data, &header));
        assert(!channel.frame.buffer.size);
        IHS_BufferClear(&data, false);
        header.flags = 0;
        assert(AppendToFrameBuffer(&channel, &data, &header));
        assert(channel.frame.hasPacketBuffers && !channel.frame.buffer.size);
        IHS_BufferAppendUInt8(&data, 0x65);
        header.flags = VideoFrameFlagNeedEscape | VideoFrameFlagNeedStartSequence;
        assert(!AppendToFrameBuffer(&channel, &data, &header));
        assert(!channel.frame.buffer.size);
        channel.frame.hasPacketBuffers = false;
        assert(AppendToFrameBuffer(&channel, &data, &header));
        assert(channel.frame.buffer.size == 5);
        header.flags = 0;
        assert(!AppendToFrameBuffer(&channel, &data, &header));
        assert(channel.frame.buffer.size == 5 && !channel.frame.hasPacketBuffers);
        channel.frame.buffer.maxCapacity = 5;
        header.flags = VideoFrameFlagNeedEscape;
        assert(!AppendToFrameBuffer(&channel, &data, &header));
        assert(channel.frame.buffer.size == 5);
        IHS_BufferClear(&data, true);
        IHS_BufferClear(&channel.frame.buffer, true);
    }
    IHS_Quit();
    puts("NAL local guards passed");
}
int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--guards")) { guards(); return 0; }
    assert(argc == 3 && (atoi(argv[1]) == 4 || atoi(argv[1]) == 5));
    char *args[] = {argv[0], "1", "0", "1", argv[2], argv[1]};
    return mode_vectors_main(6, args);
}
