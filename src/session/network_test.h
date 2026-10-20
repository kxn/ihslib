/* SPDX-License-Identifier: LGPL-3.0-or-later */
#pragma once
#include "ihslib/network_test.h"
#include "packet.h"
struct IHS_SessionFrame;
typedef struct IHS_SessionNetworkTest {
    IHS_NetworkTestMode mode;
    IHS_NetworkTestStatus status;
    unsigned phase;
    uint32_t requestedKbps, currentKbps, previousKbps, lastData;
    uint32_t handshakeTick, frameEventsTick, keepAliveTick;
    float loss;
    unsigned result;
} IHS_SessionNetworkTest;
bool IHS_SessionNetworkTestBegin(IHS_Session *);
void IHS_SessionNetworkTestConnected(IHS_Session *, uint32_t now);
void IHS_SessionNetworkTestAuthenticated(IHS_Session *);
void IHS_SessionNetworkTestRecord(IHS_Session *, struct IHS_SessionFrame *);
void IHS_SessionNetworkTestTarget(IHS_Session *, uint32_t);
void IHS_SessionNetworkTestUpdate(IHS_Session *, uint32_t);
void IHS_SessionNetworkTestCancel(IHS_Session *);
void IHS_SessionNetworkTestClosed(IHS_Session *);
void IHS_SessionNetworkTestFinalize(IHS_Session *);
void IHS_SessionNetworkTestFail(IHS_Session *);
