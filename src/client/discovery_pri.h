#pragma once

#include "client_pri.h"

/* Original CUniformRandomStream's shuffled Park-Miller stream. Private to
 * discovery; neither authentication entropy nor an application diagnostic API. */
typedef struct IHS_DiscoveryRandom {
    int32_t seed, previous, shuffle[32];
} IHS_DiscoveryRandom;

void IHS_DiscoveryRandomSeed(IHS_DiscoveryRandom *random, int32_t seed);
int IHS_DiscoveryRandomInt(IHS_DiscoveryRandom *random, int low, int high);

typedef struct IHS_DiscoveryContext {
    IHS_Client *client;
    uint32_t interval;
    uint64_t receiveGeneration, nextSend;
    bool hasSent, rounds;
    int64_t full, fast;
    unsigned tries, broadcastOwners;
    IHS_DiscoveryRandom random;
} IHS_DiscoveryContext;
