/* Observe production partial-range state and terminal submit identity. */
#include "test_session.h"
#include "session/session_pri.h"
#include <assert.h>
static IHS_Session *rangeSession(void) {
    IHS_Session *s = IHS_TestSessionCreate();
    assert(s);
    for (unsigned n = 0; n < 16; ++n) s->info.sessionKey[n] = n;
    s->info.sessionKeyLen = 16;
    return s;
}
#define IHS_SUBFRAME_OBSERVER 1
#define IHS_TestSessionCreate rangeSession
#define main range_vectors_main
#include "video_content_mode_oracle.c"
#undef main
#undef IHS_TestSessionCreate
int main(int argc, char **argv) {
    assert(argc == 4 && (atoi(argv[1]) == 4 || atoi(argv[1]) == 5));
    char *args[] = {argv[0], argv[3], atoi(argv[3]) ? "0" : "-1", "1", argv[2], argv[1]};
    return range_vectors_main(6, args);
}
