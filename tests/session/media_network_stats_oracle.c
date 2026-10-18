/* Original-method fixture inputs; real production collector/public snapshot. */
#include "ihslib.h"
#include "media_network_snapshot.h"
#include "test_session.h"
#include <stdlib.h>
static void generated(IHS_MediaNetworkStats *s, unsigned i, unsigned pattern) {
    const uint32_t values[] = {65, 200, 300, 1, 2147483647, 0, 65535, 65536, 1234567};
    uint32_t ticks = pattern ? values[i % 9] : 65;
    uint32_t sent = 1000000u + i * 1000u, received = sent + ticks;
    if (pattern == 2 && i % 3 == 0)
        received = sent - 10;
    if (pattern == 3)
        received = sent + ((i * 1103515245u + 12345u) & 0x7fffffffu);
    assert(IHS_MediaNetworkStatsRecord(s, (uint16_t)i, sent, received));
}
int main(void) {
    IHS_Init();
    IHS_Session *session = IHS_TestSessionCreate();
    assert(session);
    IHS_MediaNetworkStats *s = &session->mediaNetworkStats;
    char line[256];
    while (fgets(line, sizeof(line), stdin)) {
        unsigned a, b, c;
        switch (line[0]) {
        case 'Z':
            IHS_MediaNetworkStatsStart(s);
            break;
        case 'E':
            assert(IHS_MediaNetworkStatsStop(s));
            break;
        case 'F':
            assert(IHS_MediaNetworkStatsFold(s));
            break;
        case 'Q':
            break;
        case 'R':
            assert(sscanf(line, "R %u %u %u", &a, &b, &c) == 3);
            assert(IHS_MediaNetworkStatsRecord(s, a, b, c));
            break;
        case 'N':
            assert(sscanf(line, "N %u %u %u", &a, &b, &c) == 3);
            for (unsigned i = a; i < a + b; ++i)
                generated(s, i, c);
            break;
        case 'T': {
            assert(sscanf(line, "T %u %u %u", &a, &b, &c) == 3);
            float mean, dev, loss;
            memcpy(&mean, &a, 4);
            memcpy(&dev, &b, 4);
            memcpy(&loss, &c, 4);
            printf("%u\n", IHS_MediaNetworkStatsResult(mean, dev, loss));
            continue;
        }
        default:
            abort();
        }
        quality_report(session);
        puts("");
    }
    IHS_SessionDestroy(session);
    IHS_Quit();
    return 0;
}
