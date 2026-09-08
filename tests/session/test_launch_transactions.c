/* Real UDP request/cancel output plus injected adversarial host callbacks. */
#include "client/client_pri.h"
#include "test_session.h"
#include <arpa/inet.h>
#include <assert.h>
#include <string.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
static unsigned progress, authorized;
static atomic_int failures;
static void on_failure(IHS_Client *c, const IHS_HostInfo *h, IHS_StreamingResult result, void *ctx) {
    (void)c; (void)h; (void)ctx;
    assert(result == IHS_StreamingTimeout);
    atomic_fetch_add(&failures, 1);
}
static void on_progress(IHS_Client *c, const IHS_HostInfo *h, void *ctx) {
    (void)c; (void)h; (void)ctx; ++progress;
}
static void on_auth(IHS_Client *c, const IHS_HostInfo *h, uint64_t id, void *ctx) {
    (void)c; (void)h; (void)id; (void)ctx; ++authorized;
}
static uint32_t le(const uint8_t *b) { return b[0] | b[1]<<8 | b[2]<<16 | (uint32_t)b[3]<<24; }
static uint32_t receive_id(int fd, int expected) {
    uint8_t bytes[4096];
    ssize_t n = recv(fd, bytes, sizeof(bytes), 0);
    assert(n >= 16);
    unsigned hs = le(bytes+8), size = le(bytes+12+hs);
    assert(16+hs+size == (unsigned)n);
    CMsgRemoteClientBroadcastHeader *h = cmsg_remote_client_broadcast_header__unpack(NULL,hs,bytes+12);
    assert(h && h->msg_type == expected);
    cmsg_remote_client_broadcast_header__free_unpacked(h,NULL);
    /* Both messages use a required uint32 field 1. */
    CMsgRemoteDeviceStreamingCancelRequest *m = cmsg_remote_device_streaming_cancel_request__unpack(NULL,size,bytes+16+hs);
    assert(m); uint32_t id=m->request_id;
    cmsg_remote_device_streaming_cancel_request__free_unpacked(m,NULL);return id;
}
int main(void) {
    IHS_Init();
    int fd=socket(AF_INET,SOCK_DGRAM,0); assert(fd>=0);
    struct sockaddr_in a={.sin_family=AF_INET,.sin_addr={.s_addr=htonl(INADDR_LOOPBACK)}};
    assert(!bind(fd,(void*)&a,sizeof(a)));socklen_t size=sizeof(a);assert(!getsockname(fd,(void*)&a,&size));
    struct timeval timeout={.tv_sec=2};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
    IHS_Client *c=IHS_ClientCreate(&clientConfig);assert(c);
    IHS_HostInfo host={.clientId=23,.instanceId=45};
    IHS_IPAddressFromString(&host.address.ip,"127.0.0.1");host.address.port=ntohs(a.sin_port);
    IHS_ClientStreamingCallbacks cb={.progress=on_progress,.failed=on_failure};IHS_ClientSetStreamingCallbacks(c,&cb,NULL);
    IHS_StreamingRequest req={.gameId=2358720};
    assert(IHS_ClientStreamingRequest(c,&host,&req));uint32_t old=receive_id(fd,5);assert(old);
    assert(IHS_ClientStreamingCancel(c));assert(receive_id(fd,10)==old);
    assert(!IHS_ClientStreamingCancel(c));
    assert(IHS_ClientStreamingRequest(c,&host,&req));uint32_t current=receive_id(fd,5);assert(current && current!=old);
    CMsgRemoteClientBroadcastHeader head=CMSG_REMOTE_CLIENT_BROADCAST_HEADER__INIT;
    head.has_client_id=true;head.client_id=23;head.has_instance_id=true;head.instance_id=45;head.msg_type=6;
    CMsgRemoteDeviceStreamingResponse resp=CMSG_REMOTE_DEVICE_STREAMING_RESPONSE__INIT;
    resp.request_id=old;resp.result=k_ERemoteDeviceStreamingInProgress;
    usleep(300000);
    IHS_ClientStreamingCallback(c,&host.address,&head,(void*)&resp);
    assert(receive_id(fd,10)==old);assert(progress==0);
    resp.result=k_ERemoteDeviceStreamingSuccess;IHS_ClientStreamingCallback(c,&host.address,&head,(void*)&resp);
    assert(progress==0);
    resp.request_id=current;resp.result=k_ERemoteDeviceStreamingInProgress;
    IHS_SocketAddress wrong=host.address;++wrong.port;
    IHS_ClientStreamingCallback(c,&wrong,&head,(void*)&resp);assert(progress==0);
    head.client_id=24;IHS_ClientStreamingCallback(c,&host.address,&head,(void*)&resp);assert(progress==0);
    head.client_id=23;head.instance_id=46;IHS_ClientStreamingCallback(c,&host.address,&head,(void*)&resp);assert(progress==0);
    head.instance_id=45;IHS_ClientStreamingCallback(c,&host.address,&head,(void*)&resp);assert(progress==1);
    /* A proof without optional request_id is accepted from this host only. */
    head.msg_type=7;CMsgRemoteDeviceProofRequest proof=CMSG_REMOTE_DEVICE_PROOF_REQUEST__INIT;
    uint8_t challenge[16]={0};proof.challenge.data=challenge;proof.challenge.len=sizeof(challenge);
    proof.has_update_secret=true;proof.update_secret=true;
    IHS_ClientStreamingCallback(c,&host.address,&head,(void*)&proof);
    uint8_t bytes[4096];ssize_t n=recv(fd,bytes,sizeof(bytes),0);assert(n>16);
    unsigned hs=le(bytes+8), ps=le(bytes+12+hs);
    CMsgRemoteDeviceProofResponse *pr=cmsg_remote_device_proof_response__unpack(NULL,ps,bytes+16+hs);
    assert(pr && !pr->has_request_id && pr->has_updated_secret && !pr->updated_secret);
    assert(pr->response.len>0);cmsg_remote_device_proof_response__free_unpacked(pr,NULL);
    /* Old Progress messages must not postpone the new transaction's timeout. */
    head.msg_type=k_ERemoteDeviceStreamingProgress;
    CMsgRemoteDeviceStreamingProgress stale=CMSG_REMOTE_DEVICE_STREAMING_PROGRESS__INIT;
    stale.request_id=old;
    for (int i=0;i<10 && !atomic_load(&failures);++i) {
        usleep(2000000);
        IHS_ClientStreamingCallback(c,&host.address,&head,(void*)&stale);
    }
    assert(atomic_load(&failures)==1);
    assert(IHS_ClientStreamingCancel(c));
    /* Timed request retries may precede cancellation on the host socket. */
    uint8_t discard[4096];
    while (recv(fd,discard,sizeof(discard),MSG_DONTWAIT)>0) {}

    /* Cancel and immediately start another authorization: no dead timer owner. */
    IHS_ClientAuthorizationCallbacks acb={.success=on_auth};IHS_ClientSetAuthorizationCallbacks(c,&acb,NULL);
    assert(IHS_ClientAuthorizationRequest(c,&host,"1234"));IHS_ClientAuthorizationCancel(c);
    assert(IHS_ClientAuthorizationRequest(c,&host,"5678"));
    head.msg_type=4;CMsgRemoteDeviceAuthorizationResponse ar=CMSG_REMOTE_DEVICE_AUTHORIZATION_RESPONSE__INIT;
    ar.result=k_ERemoteDeviceAuthorizationSuccess;ar.steamid=1;
    IHS_ClientAuthorizationCallback(c,&wrong,&head,(void*)&ar);assert(!authorized);
    IHS_ClientAuthorizationCallback(c,&host.address,&head,(void*)&ar);assert(authorized==1);
    IHS_ClientAuthorizationCancel(c);
    IHS_ClientStop(c);IHS_ClientThreadedJoin(c);IHS_ClientDestroy(c);close(fd);IHS_Quit();
}
