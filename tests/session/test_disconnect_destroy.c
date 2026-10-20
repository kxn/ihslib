/*
 *  _____  _   _  _____  _  _  _
 * |_   _|| | | |/  ___|| |(_)| |     Steam
 *   | |  | |_| |\ `--. | | _ | |__     In-Home
 *   | |  |  _  | `--. \| || || '_ \      Streaming
 *  _| |_ | | | |/\__/ /| || || |_) |       Library
 *  \___/ \_| |_/\____/ |_||_||_.__/
 *
 * Copyright (c) 2026 Mariotaku <https://github.com/mariotaku>.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 */

// Regression coverage for repeated close, immediate destruction and worker join.
// The old DisconnectTimerEnd UAF path (c516f06) no longer has a timer: official
// Client::Disconnect attempts one goodbye and closes. Preserve destruction
// coverage while ordinary feedback timers and workers may still be active.

#include <unistd.h>
#include <assert.h>
#include <signal.h>
#include <stdio.h>

#include "session/session_pri.h"
#include "session/channels/channel.h"
#include "session/channels/ch_discovery.h"
#include "session/channels/ch_control.h"
#include "test_session.h"

static void test_destroy_immediately_after_disconnect(void) {
    IHS_Session *session = IHS_TestSessionCreate();
    IHS_SessionChannel *discovery = IHS_SessionChannelFor(session, IHS_SessionChannelIdDiscovery);
    IHS_SessionChannelDiscoveryDisconnect(discovery);
    /* Duplicate stop requests must be idempotent. */
    IHS_SessionChannelDiscoveryDisconnect(discovery);
    // Destroy immediately after close, without relying on a timer callback.
    IHS_SessionDestroy(session);
}

static void test_destroy_after_brief_yield(void) {
    IHS_Session *session = IHS_TestSessionCreate();
    IHS_SessionChannel *discovery = IHS_SessionChannelFor(session, IHS_SessionChannelIdDiscovery);
    IHS_SessionChannelDiscoveryDisconnect(discovery);
    // Allow normal background feedback work before destruction.
    usleep(20 * 1000);
    IHS_SessionDestroy(session);
}

static void test_threaded_disconnect_without_host_ack(void) {
    IHS_Session *session = IHS_TestSessionCreate();
    assert(IHS_SessionConnect(session));

    // Exercise close/join with no peer response. Generic cleanup sends no 129.
    usleep(20 * 1000);
    alarm(4);
    IHS_SessionDisconnect(session);
    IHS_SessionThreadedJoin(session);
    IHS_SessionDestroy(session);
    alarm(0);
}

static int host_stop_calls;
static void host_stopped(IHS_Session *session, void *context) {
    (void)context;
    assert(IHS_SessionHostRequestedStop(session));
    ++host_stop_calls;
}
static void test_explicit_host_stop(void) {
    IHS_Session *session = IHS_TestSessionCreate();
    assert(!IHS_SessionHostRequestedStop(session));
    IHS_StreamSessionCallbacks cb = {.disconnected = host_stopped};
    IHS_SessionSetSessionCallbacks(session, &cb, NULL);
    IHS_SessionHostStopped(session);
    assert(host_stop_calls >= 1);
    assert(IHS_SessionHostRequestedStop(session));
    IHS_SessionDestroy(session);
    session = IHS_TestSessionCreate();
    IHS_SessionChannel *control = IHS_SessionChannelFor(session, IHS_SessionChannelIdControl);
    uint16_t before = control->nextPacketId;
    IHS_SessionDisconnect(session);
    assert(control->nextPacketId == before); /* no StopRequest / game termination */
    assert(!IHS_SessionHostRequestedStop(session));
    IHS_SessionDestroy(session);
}
int main(void) {
    IHS_Init();
    test_explicit_host_stop();
    for (int i = 0; i < 5; ++i) {
        test_destroy_immediately_after_disconnect();
    }
    for (int i = 0; i < 5; ++i) {
        test_destroy_after_brief_yield();
    }
    test_threaded_disconnect_without_host_ack();
    IHS_Quit();
    printf("disconnect lifecycle tests OK\n");
    return 0;
}
