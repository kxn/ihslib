/* Real BaseWorker cycle dispatch, CRC RX, reliable delivery and feedback;
 * scripted IO costs match official batch_2 vectors. Separately test actual
 * POSIX readiness/nonblocking drain on loopback (no external peer). */
#include "ihslib.h"
#include "ihs_buffer.h"
#include "ihs_buffer_ext.h"
#include "session/session_pri.h"
#include "session/reliable.h"
#include "session/channels/ch_data.h"
#include "session/channels/ch_control.h"
#include "test_session.h"
#include <assert.h>
#include <netinet/in.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <pthread.h>
#include <unistd.h>

static IHS_Session *s;
static atomic_uint now, work, phase, frames, handshakeId, postBatchPolls;
static atomic_bool scripted, handshake, acknowledged;
static uint32_t start, batch;
static bool crc;
static bool ordinary;
static bool controlQueue;
static bool interruptQueue;
static atomic_bool controlAckSeen;
static IHS_SessionPacketsWindow *dataWindow;
bool __real_IHS_SessionPacketsWindowPoll(IHS_SessionPacketsWindow *, IHS_SessionFrame *);
bool __wrap_IHS_SessionPacketsWindowPoll(IHS_SessionPacketsWindow *window, IHS_SessionFrame *f) {
    bool result = __real_IHS_SessionPacketsWindowPoll(window, f);
    if (ordinary && window == dataWindow && atomic_load(&acknowledged)) {
        /* Poll may retry an empty window within the same cycle, or run in a
         * later BaseWorker cycle. Only the latter captures the post-drain
         * clock. Main waits for two later empty polls before teardown. */
        assert(!result && atomic_load(&phase) == 7 && atomic_load(&now) == batch + 136);
        if (s->transport.updateTime != batch + 36) {
            assert(s->transport.updateTime == batch + 136);
            atomic_fetch_add(&postBatchPolls, 1);
        }
    } else if (ordinary && window == dataWindow) {
        assert(atomic_load(&phase) == 7 && atomic_load(&now) == batch + 136);
        assert(s->transport.updateTime == batch + 36);
        if (result) {
            unsigned n = atomic_fetch_add(&frames, 1);
            assert(n < 2 && f->header.receiveBegin == batch+n*18 &&
                f->header.receiveTimestamp == batch+n*18 && f->header.sendTimestamp == 900000+n);
        } else if (atomic_load(&frames) == 2) atomic_store(&acknowledged, true);
    }
    return result;
}
static void data_init(IHS_SessionChannel *ch, const void *unused) {
    (void)unused; assert(IHS_SessionChannelDataInit(ch, 32));
}
static bool data_start(IHS_SessionChannel *ch) { (void)ch; return true; }
static void data_stop(IHS_SessionChannel *ch) { (void)ch; }
static const IHS_SessionChannelDataClass dataClass = {
    .base={.instanceSize=sizeof(IHS_SessionChannelData),.init=data_init,
        .deinit=IHS_SessionChannelDataDeinit,.stopped=IHS_SessionChannelDataStopped,
        .received=IHS_SessionChannelDataReceived},.start=data_start,.stop=data_stop,
};
uint32_t __wrap_IHS_SessionPacketTimestamp(void) { return atomic_load(&now); }
static unsigned read32(const uint8_t *p) {
    return p[0] | (unsigned)p[1]<<8 | (unsigned)p[2]<<16 | (unsigned)p[3]<<24;
}
IHS_SessionPacketReturn __real_IHS_SessionPacketParse(IHS_SessionPacket *, IHS_Buffer *);
IHS_SessionPacketReturn __wrap_IHS_SessionPacketParse(IHS_SessionPacket *p, IHS_Buffer *b) {
    IHS_SessionPacketReturn result=__real_IHS_SessionPacketParse(p,b);
    atomic_fetch_add(&now,atomic_exchange(&work,0)); return result;
}
IHS_ReliableDisposition __real_IHS_SessionDeliverReliableFrame(IHS_Session *, IHS_SessionFrame *);
IHS_ReliableDisposition __wrap_IHS_SessionDeliverReliableFrame(IHS_Session *session,IHS_SessionFrame *f) {
    if (f->header.channelId==4 || (controlQueue && f->header.channelId==1)) {
        unsigned n=atomic_load(&frames);
        assert(n<2 && f->header.receiveBegin==batch+n*18 && f->header.receiveTimestamp==batch+n*18);
        atomic_fetch_add(&frames,1);
    }
    return __real_IHS_SessionDeliverReliableFrame(session,f);
}
static void *probe_consumer_locks(void *unused) {
    (void)unused;
    IHS_BaseLock(&s->base);
    IHS_SessionChannelControl *control=(void *)s->channels[1];
    IHS_MutexLock(control->incomingLock);
    IHS_MutexUnlock(control->incomingLock);
    IHS_BaseUnlock(&s->base);
    return NULL;
}
void __real_IHS_SessionChannelControlOnMessageReceived(IHS_SessionChannel *,EStreamControlMessage,
    IHS_Buffer *,const IHS_SessionPacketHeader *);
void __wrap_IHS_SessionChannelControlOnMessageReceived(IHS_SessionChannel *ch,EStreamControlMessage type,
    IHS_Buffer *body,const IHS_SessionPacketHeader *header) {
    if(controlQueue && type==k_EStreamControlSetInputTemporarilyDisabled) {
        assert(!interruptQueue);
        assert(atomic_load(&controlAckSeen) && !s->state.inputTemporarilyDisabled);
        assert(atomic_load(&phase)==7 && atomic_load(&frames)==2);
        /* A real handler can join a consumer which still needs Base. Verify
         * both network and queue locks are released before dispatch. */
        pthread_t probe;assert(!pthread_create(&probe,NULL,probe_consumer_locks,NULL));
        assert(!pthread_join(probe,NULL));
    }
    __real_IHS_SessionChannelControlOnMessageReceived(ch,type,body,header);
    if(controlQueue && type==k_EStreamControlSetInputTemporarilyDisabled) {
        assert(s->state.inputTemporarilyDisabled);
        atomic_store(&acknowledged,true);
    }
}
bool __real_IHS_BaseSend(IHS_Base *,IHS_SocketAddress,const IHS_Buffer *);
bool __wrap_IHS_BaseSend(IHS_Base *base,IHS_SocketAddress address,const IHS_Buffer *b) {
    if (!atomic_load(&scripted)) return __real_IHS_BaseSend(base,address,b);
    const uint8_t *w=IHS_BufferPointer(b); unsigned type=w[0]&127;
    assert(!ordinary || type != 10 || w[4] != 3); /* complete batch needs no NACK */
    if (type==5 && w[4]==1) {
        atomic_store(&handshakeId,w[7]|(unsigned)w[8]<<8);atomic_store(&handshake,true);
    }
    if (type==7 && w[4]==4) {
        assert(atomic_load(&frames)==2 && atomic_load(&phase)==7);
        assert(w[7]==1 && !w[8] && read32(w+9)==batch+136 && read32(w+13)==900119);
        assert(s->transport.updateTime==batch+36);
        atomic_store(&acknowledged,true);
    }
    if(controlQueue && type==7 && w[4]==1 && atomic_load(&phase)==7) {
        IHS_SessionChannelControl *control=(void *)s->channels[1];
        assert(control->recvEncryptSequence==2 && control->incomingCount==1);
        assert(!s->state.inputTemporarilyDisabled && atomic_load(&frames)==2);
        assert(read32(w+9)==batch+136 && read32(w+13)==900119);
        atomic_store(&controlAckSeen,true);
        if(interruptQueue) {
            /* Local stop between network feedback and client consumption must
             * leave the queued action for destruction, not restart consumers. */
            IHS_SessionInterrupt(s);
            atomic_store(&acknowledged,true);
        }
    }
    return true;
}
int __real_IHS_UDPSocketWaitReadable(IHS_UDPSocket *,uint32_t);
int __wrap_IHS_UDPSocketWaitReadable(IHS_UDPSocket *socket,uint32_t ms) {
    if (!atomic_load(&scripted)) return __real_IHS_UDPSocketWaitReadable(socket,ms);
    assert(ms==0 || ms==2);
    if (atomic_load(&phase)==4) atomic_store(&now,batch);
    return 1;
}
static int incoming(IHS_UDPPacket *rx,unsigned type,unsigned ch,unsigned id,unsigned retry) {
    IHS_SessionPacket p={.header={.type=type,.channelId=ch,.packetId=id,.retransmitCount=retry,
        .srcConnectionId=34,.dstConnectionId=s->state.connectionId,.hasCrc=type!=2&&crc,
        .sendTimestamp=type==5||type==3?900000+id:atomic_load(&now)}};
    IHS_SessionPacketBodyInitialize(&p.body,p.header.hasCrc);
    uint8_t bytes[4]={0};size_t size=4;
    if(type==5||type==3) {bytes[0]=6;bytes[1]=0xaa;bytes[2]=0xbb;size=3;}
    if(type==7) {uint32_t t=atomic_load(&now);for(unsigned n=0;n<4;++n)bytes[n]=t>>(n*8);}
    if(controlQueue && type==5 && ch==1) {
        if(id==1) {
            const uint8_t plain[]={0x08,0x01};uint8_t cipher[64];size_t cipherSize=sizeof(cipher);
            assert(!IHS_SessionFrameEncrypt(s,plain,sizeof(plain),cipher,&cipherSize,1));
            IHS_BufferAppendUInt8(&p.body,k_EStreamControlSetInputTemporarilyDisabled);
            IHS_BufferAppendMem(&p.body,cipher,cipherSize);
        }
    } else IHS_BufferAppendMem(&p.body,bytes,size);
    IHS_SessionPacketPopulateBuffer(&p);
    rx->address=s->info.address;IHS_BufferAppendMem(&rx->buffer,p.body.data,IHS_SessionPacketSize(&p));
    IHS_SessionPacketClear(&p,true);return 1;
}
int __real_IHS_UDPSocketReceive(IHS_UDPSocket *,IHS_UDPPacket *);
int __wrap_IHS_UDPSocketReceive(IHS_UDPSocket *socket,IHS_UDPPacket *rx) {
    if(!atomic_load(&scripted))return __real_IHS_UDPSocketReceive(socket,rx);
    switch(atomic_load(&phase)) {
    case 0:atomic_store(&phase,1);return incoming(rx,2,0,0,0);
    case 1:atomic_store(&phase,2);return 0;
    case 2:
        if(!atomic_load(&handshake)){usleep(1000);return 0;}
        atomic_store(&phase,3);return incoming(rx,7,1,atomic_load(&handshakeId),0);
    case 3:atomic_store(&phase,4);return 0;
    case 4:case 5: {
        unsigned n=atomic_load(&phase)-4;assert(!atomic_load(&frames));
        if(controlQueue) {
            s->state.connectionState=IHS_SessionConnectionStateConnected;
            IHS_SessionChannelControl *control=(void *)s->channels[1];
            control->keepAliveTimestamp=control->frameEventsTimestamp=batch;
            assert(!s->state.inputTemporarilyDisabled);
        }
        atomic_fetch_add(&now,7);atomic_store(&work,11);atomic_fetch_add(&phase,1);
        return incoming(rx,ordinary?3:5,controlQueue?1:ordinary?3:4,n,n);
    }
    case 6:
        assert(!atomic_load(&frames));atomic_fetch_add(&now,100);atomic_store(&phase,7);return 0;
    default:usleep(1000);return 0;
    }
}
static void loopback(void) {
    int server=socket(AF_INET,SOCK_DGRAM,0);assert(server>=0);
    struct sockaddr_in addr={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
    assert(!bind(server,(void *)&addr,sizeof(addr)));socklen_t size=sizeof(addr);
    assert(!getsockname(server,(void *)&addr,&size));
    struct timeval timeout={.tv_sec=1};assert(!setsockopt(server,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout)));
    IHS_UDPSocket *client=IHS_UDPSocketOpen(false);assert(client&&IHS_UDPSocketSetBlocking(client,false));
    assert(!IHS_UDPSocketWaitReadable(client,0));
    uint8_t byte=0;IHS_UDPPacket tx={.buffer=IHS_BUFFER_WRAP(&byte,1)};
    assert(IHS_IPAddressFromString(&tx.address.ip,"127.0.0.1"));tx.address.port=ntohs(addr.sin_port);
    assert(IHS_UDPSocketSend(client,&tx));struct sockaddr_in peer;socklen_t peerSize=sizeof(peer);
    assert(recvfrom(server,&byte,1,0,(void *)&peer,&peerSize)==1);
    for(byte=1;byte<=2;++byte)assert(sendto(server,&byte,1,0,(void *)&peer,peerSize)==1);
    assert(IHS_UDPSocketWaitReadable(client,10)==1);
    IHS_UDPPacket rx={.buffer=IHS_BUFFER_INIT(2048,2048)};
    for(unsigned n=1;n<=2;++n){assert(IHS_UDPSocketReceive(client,&rx)==1);
        assert(rx.buffer.size==1&&*IHS_BufferPointer(&rx.buffer)==n);IHS_BufferClear(&rx.buffer,false);}
    assert(!IHS_UDPSocketReceive(client,&rx));assert(!IHS_UDPSocketWaitReadable(client,0));
    IHS_BufferClear(&rx.buffer,true);IHS_UDPSocketClose(client);close(server);
}
int main(void) {
    IHS_Init();loopback();
    for(unsigned mode=0;mode<4;++mode)for(unsigned c=0;c<2;++c){
        ordinary=mode==1;controlQueue=mode>=2;interruptQueue=mode==3;dataWindow=NULL;
        atomic_store(&controlAckSeen,false);
        crc=c;start=1000000;batch=start+1000;atomic_store(&now,start);
        atomic_store(&phase,0);atomic_store(&frames,0);atomic_store(&work,0);
        atomic_store(&postBatchPolls,0);
        atomic_store(&handshake,false);atomic_store(&acknowledged,false);atomic_store(&scripted,true);
        s=IHS_TestSessionCreate();s->outgoing.rate[0]=s->outgoing.rate[1]=0;
        if (ordinary) {
            IHS_SessionChannelData *d=(void *)IHS_SessionChannelDataCreate(&dataClass,s,
                IHS_SessionChannelTypeDataVideo,3,NULL);
            assert(d);dataWindow=d->receive->window;IHS_SessionChannelAdd(s,&d->base);
        }
        assert(IHS_SessionConnect(s));uint64_t end=IHS_TimerNow()+2000;
        while(!atomic_load(&acknowledged)&&IHS_TimerNow()<end)usleep(1000);
        assert(atomic_load(&acknowledged));
        if (ordinary) {
            while (atomic_load(&postBatchPolls) < 2 && IHS_TimerNow() < end) usleep(1000);
            assert(atomic_load(&postBatchPolls) >= 2);
        }
        IHS_BaseLock(&s->base);assert(s->networkPackets.lost==(ordinary?0u:1u));
        bool found=false;for(size_t n=0;n<s->networkPackets.queue.count;++n){
            const IHS_NetworkSample *p=&s->networkPackets.queue.data[(s->networkPackets.queue.head+n)&(s->networkPackets.queue.capacity-1)];
            if(p->values[1]){assert(p->timestamp==batch+36&&p->values[0]==1);found=true;}}
        assert(found==!ordinary);IHS_BaseUnlock(&s->base);
        IHS_SessionDisconnect(s);IHS_SessionThreadedJoin(s);IHS_SessionDestroy(s);
        atomic_store(&scripted,false);
    }
    IHS_Quit();return 0;
}
