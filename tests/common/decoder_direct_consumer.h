/* Direct consumer fixtures explicitly drive their Source OnThink boundaries.
 * Hold the video OS worker at startup so it cannot add an unrequested think.
 * Production caller-start behavior retains the backend for these fixtures. */
#pragma once
#include "ihs_thread.h"
#include <string.h>
#ifndef IHS_DECODER_EXTERNAL_THREAD_FACTORY
IHS_Thread *__real_IHS_ThreadCreate(IHS_ThreadFunction *, const char *, void *);
IHS_Thread *__wrap_IHS_ThreadCreate(IHS_ThreadFunction *fn, const char *name, void *ctx) {
    if (!strcmp(name, "IHSVideo"))
        return NULL;
    return __real_IHS_ThreadCreate(fn, name, ctx);
}
#endif
