/* Test-only serialization of private state and validation of public snapshot. */
#pragma once
#include "session/session_pri.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static void quality_hex(const void *data, size_t size) {
    const unsigned char *p = data;
    for (size_t i = 0; i < size; ++i)
        printf("%02x", p[i]);
}
static void quality_report(IHS_Session *session) {
    IHS_MediaNetworkStats *s = &session->mediaNetworkStats;
    IHS_SessionMediaNetworkStats out;
    IHS_SessionGetMediaNetworkStats(session, &out);
    IHS_MediaNetworkWindow current = IHS_MediaNetworkStatsCurrent(s);
    assert(out.nextFrameId == s->next && out.frameSpan == s->total);
    assert(out.windowFrameSpan == s->batch && out.windowReceived == s->count);
    assert(out.windows == s->windowCount && out.valid == s->valid && out.finalized == s->finalized);
    assert(!memcmp(&out.windowLossPercent, &current, sizeof(current)));
    assert(!memcmp(&out.lossPercent, &s->average, sizeof(s->average)));
    printf("{\"expected\":%u,\"total\":%u,\"batch\":%u,\"latencies\":[", s->next, s->total,
           s->batch);
    for (size_t i = 0; i < s->count; ++i)
        printf("%s%u", i ? "," : "", s->latencies[i]);
    printf("],\"samples\":[");
    for (size_t i = 0; i < s->windowCount; ++i) {
        printf("%s\"", i ? "," : "");
        quality_hex(&s->windows[i], 12);
        printf("\"");
    }
    printf("],\"average\":\"");
    quality_hex(&s->average, 12);
    printf("\",\"result\":%u,\"current\":\"", out.result);
    quality_hex(&current, 12);
    printf("\"}");
}
