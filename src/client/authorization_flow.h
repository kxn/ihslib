#pragma once
#include "authorization_kx.h"
#include "ihslib/client.h"
#include "protobuf/discovery.pb-c.h"

/* Private request material survives authorization -> stream -> reauthorization.
 * The most-recent public key/result remains in IHS_Client, independently. */
typedef struct IHS_AuthorizationFlow {
    char pin[16];
    IHS_AuthorizationKeyExchange keyExchange;
    unsigned keyExchangeAttempts;
} IHS_AuthorizationFlow;

typedef enum IHS_AuthorizationExchangeOutcome {
    IHS_AuthorizationExchangeWaiting,
    IHS_AuthorizationExchangeAccepted,
    IHS_AuthorizationExchangeExhausted,
    IHS_AuthorizationExchangeResourceFailure,
} IHS_AuthorizationExchangeOutcome;

bool IHS_ClientAuthorizationFlowInit(IHS_AuthorizationFlow *, const char *pin);
bool IHS_ClientSendAuthorizationRequest(IHS_Client *, const IHS_HostInfo *, IHS_AuthorizationFlow *,
                                        uint32_t requestId);
IHS_AuthorizationExchangeOutcome
IHS_ClientAuthorizationExchange(IHS_Client *, const IHS_HostInfo *, IHS_AuthorizationFlow *,
                                const CMsgRemoteDeviceAuthorizationResponse *);
void IHS_ClientAuthorizationSendCancel(IHS_Client *, const IHS_HostInfo *);
void IHS_ClientAuthorizationConfirm(IHS_Client *, IHS_SocketAddress,
                                    ERemoteDeviceAuthorizationResult);
