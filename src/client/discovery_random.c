#include "discovery_pri.h"

void IHS_DiscoveryRandomSeed(IHS_DiscoveryRandom *random, int32_t seed) {
    /* SetSeed stores a nonpositive seed. */
    random->seed = seed > 0 ? -seed : seed;
    random->previous = 0;
}

static int32_t Next(IHS_DiscoveryRandom *random) {
    int64_t seed = random->seed;
    if (seed <= 0 || !random->previous) {
        /* ARM's 32-bit negation retains INT32_MIN. The wide Schrage step
         * below reproduces its first signed division without C overflow. */
        seed = seed < 0 ? (seed == INT32_MIN ? seed : -seed) : 1;
        for (int j = 39; j >= 0; --j) {
            seed = 16807 * (seed % 127773) - 2836 * (seed / 127773);
            if (seed < 0) seed += INT32_MAX;
            if (j < 32) random->shuffle[j] = (int32_t)seed;
        }
        random->previous = random->shuffle[0];
    }
    seed = 16807 * (seed % 127773) - 2836 * (seed / 127773);
    if (seed < 0) seed += INT32_MAX;
    unsigned index = (unsigned)random->previous / 67108864u;
    random->previous = random->shuffle[index];
    random->shuffle[index] = (int32_t)seed;
    random->seed = (int32_t)seed;
    return random->previous;
}

int IHS_DiscoveryRandomInt(IHS_DiscoveryRandom *random, int low, int high) {
    if (high <= low) return low;
    uint32_t width = (uint32_t)(high - low) + 1u;
    uint32_t limit = INT32_MAX - (0x80000000u % width);
    uint32_t value;
    do { value = (uint32_t)Next(random); } while (value > limit);
    return low + (int)(value % width);
}
