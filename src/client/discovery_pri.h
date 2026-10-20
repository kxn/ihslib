#pragma once

#include "client_pri.h"

/* Original CUniformRandomStream's shuffled Park-Miller stream. Private to
 * discovery; neither authentication entropy nor an application diagnostic API. */
typedef struct IHS_DiscoveryRandom {
    int32_t seed, previous, shuffle[32];
} IHS_DiscoveryRandom;

void IHS_DiscoveryRandomSeed(IHS_DiscoveryRandom *random, int32_t seed);
int IHS_DiscoveryRandomInt(IHS_DiscoveryRandom *random, int low, int high);

typedef enum IHS_DiscoveryOperation {
    IHS_DiscoveryEnable, IHS_DiscoveryDisable, IHS_DiscoveryTrigger, IHS_DiscoveryTargets
} IHS_DiscoveryOperation;

typedef struct IHS_DiscoveryCommand {
    IHS_DiscoveryOperation operation;
    IHS_DiscoveryTarget *targets;
    size_t count, idCount;
} IHS_DiscoveryCommand;

/* A local API backlog budget, not a wire/protocol limit. Rejection is atomic. */
#define IHS_DISCOVERY_COMMAND_CAPACITY 64

typedef struct IHS_DiscoveryContext {
    IHS_Client *client;
    uint32_t interval;
    uint64_t receiveGeneration, nextSend;
    bool hasSent, rounds;
    int64_t full, fast;
    unsigned tries, broadcastOwners;
    unsigned acceptedOwners;
    bool cycleActive;
    IHS_DiscoveryCommand commands[IHS_DISCOVERY_COMMAND_CAPACITY];
    unsigned commandHead, commandCount, queuedTargets;
    const IHS_DiscoveryTarget *desiredTargets;
    size_t desiredCount;
    IHS_DiscoveryRandom random;
} IHS_DiscoveryContext;
