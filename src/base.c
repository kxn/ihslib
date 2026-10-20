/*
 *  _____  _   _  _____  _  _  _
 * |_   _|| | | |/  ___|| |(_)| |     Steam
 *   | |  | |_| |\ `--. | | _ | |__     In-Home
 *   | |  |  _  | `--. \| || || '_ \      Streaming
 *  _| |_ | | | |/\__/ /| || || |_) |       Library
 *  \___/ \_| |_/\____/ |_||_||_.__/
 *
 * Copyright (c) 2022 Mariotaku <https://github.com/mariotaku>.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "base.h"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "ihs_buffer.h"
#include "session/channels/ch_control.h"

static void BaseWorker(IHS_Base *base);

static bool initialized;

void IHS_Init() {
    if (initialized)
        return;
#if IHSLIB_DIAGNOSTICS
    IHS_ControlDiagnosticsInit();
#endif
    initialized = true;
    IHS_TimerInit();
}

void IHS_Quit() {
    if (!initialized)
        return;
    IHS_TimerQuit();
#if IHSLIB_DIAGNOSTICS
    IHS_ControlDiagnosticsQuit();
#endif
    initialized = false;
}

void IHS_BaseInit(IHS_Base *base, const IHS_ClientConfig *config, IHS_BaseReceivedFunction recvCb,
                  bool broadcast) {
    assert(base != NULL);
    assert(initialized);
    memset(base, 0, sizeof(IHS_Base));
    atomic_init(&base->interrupted, false);
    atomic_init(&base->callbacks.log, NULL);
    base->broadcast = broadcast;
    base->lock = IHS_MutexCreate();
    base->callbacks.received = recvCb;

    base->deviceId = config->deviceId;
    strncpy(base->deviceName, config->deviceName ? config->deviceName : "IHSLib",
            sizeof(base->deviceName) - 1);
    base->deviceName[sizeof(base->deviceName) - 1] = '\0';
    snprintf(base->deviceModel, sizeof(base->deviceModel), "%s",
             config->deviceModel ? config->deviceModel : "");
    snprintf(base->deviceSerial, sizeof(base->deviceSerial), "%s",
             config->deviceSerial ? config->deviceSerial : "");
    base->deviceProvisioningId = config->deviceProvisioningId;
    memcpy(base->secretKey, config->secretKey, sizeof(base->secretKey));
}

void IHS_BaseSetSecretKey(IHS_Base *base, const uint8_t *secretKey) {
    assert(base != NULL);
    IHS_BaseLock(base);
    memcpy(base->secretKey, secretKey, sizeof(base->secretKey));
    IHS_BaseUnlock(base);
}

void IHS_BaseGetSecretKey(IHS_Base *base, uint8_t *secretKey) {
    assert(base != NULL);
    IHS_BaseLock(base);
    memcpy(secretKey, base->secretKey, sizeof(base->secretKey));
    IHS_BaseUnlock(base);
}

void IHS_BaseSetLogFunction(IHS_Base *base, IHS_LogFunction *logFunction) {
    assert(base != NULL);
    atomic_store_explicit(&base->callbacks.log, logFunction, memory_order_release);
}

void IHS_BaseSetRunCallbacks(IHS_Base *base, const IHS_BaseRunCallbacks *callbacks, void *context) {
    assert(base != NULL);
    IHS_BaseLock(base);
    base->callbacks.run = callbacks;
    base->callbackContexts.run = context;
    IHS_BaseUnlock(base);
}

void IHS_BaseLog(IHS_Base *base, IHS_LogLevel level, const char *tag, const char *fmt, ...) {
    assert(base != NULL);
    IHS_LogFunction *log = atomic_load_explicit(&base->callbacks.log, memory_order_acquire);
    if (!log)
        return;
    char buf[4096];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, 4095, fmt, args);
    log(level, tag, buf);
    va_end(args);
}

const char *IHS_LogLevelName(IHS_LogLevel level) {
    switch (level) {
    case IHS_LogLevelFatal:
        return "Fatal";
    case IHS_LogLevelError:
        return "Error";
    case IHS_LogLevelWarn:
        return "Warn";
    case IHS_LogLevelInfo:
        return "Info";
    case IHS_LogLevelDebug:
        return "Debug";
    case IHS_LogLevelVerbose:
        return "Verbose";
    default:
        return "";
    }
}

bool IHS_BaseStartWorker(IHS_Base *base, const char *name) {
    assert(base != NULL);
    IHS_BaseLock(base);
    bool external = base->transport.receive != NULL;
    if (base->worker != NULL || base->socket != NULL ||
        (external && (base->started || base->interrupted))) {
        IHS_BaseUnlock(base);
        return false;
    }
    /* Official discovery BStart reports failed BInit before becoming active.
     * Resolve local resources before returning success to either owner. */
    IHS_UDPSocket *socket = external ? NULL : IHS_UDPSocketOpen(base->broadcast);
    if (!external && !socket) {
        IHS_BaseUnlock(base);
        return false;
    }
    base->socket = socket;
    base->transportActive = external;
    base->worker = IHS_ThreadCreate((IHS_ThreadFunction *)BaseWorker, name, base);
    if (!base->worker) {
        base->socket = NULL;
        base->transportActive = false;
        if (socket) IHS_UDPSocketClose(socket);
        IHS_BaseUnlock(base);
        return false;
    }
    base->started = true;
    IHS_BaseLog(base, IHS_LogLevelInfo, "Worker", "Worker thread %s created", name);
    IHS_BaseUnlock(base);
    return true;
}

void IHS_BaseInterruptWorker(IHS_Base *base) {
    assert(base != NULL);
    IHS_BaseLock(base);
    if (base->interrupted) {
        IHS_BaseUnlock(base);
        return;
    }
    base->interrupted = true;
    IHS_BaseUnlock(base);
}

void IHS_BaseWaitWorker(IHS_Base *base) {
    assert(base != NULL);
    IHS_ThreadJoin(base->worker);
    base->worker = NULL;
}

void IHS_BaseDestroy(IHS_Base *base) {
    assert(base != NULL);
    IHS_MutexDestroy(base->lock);
}

bool IHS_BaseSend(IHS_Base *base, IHS_SocketAddress address, const IHS_Buffer *data) {
    assert(base != NULL);
    // Hold base->lock across the send so the worker thread's close-on-shutdown can't
    // free the socket out from under us. The worker sets socket = NULL under the same
    // lock before calling IHS_UDPSocketClose, so any send that completes here uses a
    // socket the worker has agreed to keep alive.
    IHS_BaseLock(base);
    bool ret = false;
    if (base->transportActive) {
        ret = base->transport.send(base->transportContext, IHS_BufferPointer(data), data->size);
    } else if (base->socket != NULL) {
        IHS_UDPPacket packet = {.address = address, .buffer = *data};
        ret = IHS_UDPSocketSend(base->socket, &packet);
    }
    IHS_BaseUnlock(base);
    return ret;
}

void IHS_BaseLock(IHS_Base *base) {
    assert(base != NULL);
    IHS_MutexLock(base->lock);
}

void IHS_BaseUnlock(IHS_Base *base) {
    assert(base != NULL);
    IHS_MutexUnlock(base->lock);
}

static void BaseWorker(IHS_Base *base) {
    assert(base != NULL);
    IHS_BaseLock(base);
    IHS_UDPSocket *socket = base->socket;
    IHS_BaseUnlock(base);
    if (base->callbacks.run && base->callbacks.run->initialized) {
        base->callbacks.run->initialized(base, base->callbackContexts.run);
    }
    IHS_UDPPacket recv;
    size_t receiveCapacity = base->callbacks.run && base->callbacks.run->receiveCycle
                                 ? IHS_STREAM_UDP_RECV_CAPACITY
                                 : 2048;
    if (base->callbacks.run && base->callbacks.run->receiveCapacity)
        receiveCapacity = base->callbacks.run->receiveCapacity;
    if (base->transportActive && base->transport.receiveCapacity)
        receiveCapacity = base->transport.receiveCapacity;
    IHS_BufferInit(&recv.buffer, receiveCapacity, receiveCapacity);
    for (;;) {
        IHS_BaseLock(base);
        bool interrupted = base->interrupted;
        IHS_BaseUnlock(base);
        if (interrupted)
            break;
        if (base->callbacks.run && base->callbacks.run->receiveCycle) {
            if (!base->callbacks.run->receiveCycle(base, base->callbackContexts.run, &recv))
                break;
            continue;
        }
        if (base->callbacks.run && base->callbacks.run->update)
            base->callbacks.run->update(base, base->callbackContexts.run);
        if (base->interrupted)
            break;
        int ret;
        if ((ret = IHS_UDPSocketReceive(socket, &recv)) < 0) {
            break;
        }
        if (ret) {
            base->callbacks.received(base, &recv.address, &recv.buffer);
        }
        IHS_BufferClear(&recv.buffer, false);
    }
    IHS_BufferClear(&recv.buffer, true);
    if (base->callbacks.run && base->callbacks.run->finalized) {
        base->callbacks.run->finalized(base, base->callbackContexts.run);
    }
    // Clear base->socket under the lock so any concurrent IHS_BaseSend that takes
    // the lock after this point sees NULL and bails. Then close the socket after
    // releasing the lock — at that point no in-flight send can still reference it
    // (each send takes the lock for its full duration).
    IHS_BaseLock(base);
    base->socket = NULL;
    /* Finalized closes the IHS transport and may send its one goodbye. Keep
     * either backend alive until those producers have stopped and joined. */
    base->transportActive = false;
    IHS_BaseUnlock(base);
    if (socket) IHS_UDPSocketClose(socket);
}
