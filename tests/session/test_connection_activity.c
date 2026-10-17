/* Independent clock/pause wire expectations from pinned 1.3.32;
 * see parent STEAMLINK_PROTOCOL_RE connection inactivity specification. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "common/test_session.h"
#include "ihslib.h"
#include "ihs_buffer_ext.h"
#include "session/channels/ch_control.h"
#include "protobuf/remoteplay.pb-c.h"

struct IHS_QueueItem { IHS_SessionPacket packet; bool reliable; };
static unsigned pauses, resumes;
bool __wrap_IHS_SessionChannelControlSend(IHS_SessionChannel *channel, EStreamControlMessage type,
                                         const ProtobufCMessage *message, int32_t packet_id) {
    assert(channel->id==IHS_SessionChannelIdControl && packet_id==IHS_PACKET_ID_NEXT);
    if (type==k_EStreamControlPause) { assert(message->descriptor==&cpause_msg__descriptor); ++pauses; }
    else if (type==k_EStreamControlResume) { assert(message->descriptor==&cresume_msg__descriptor); ++resumes; }
    else assert(0);
    return true;
}
static IHS_Session *new_session(void) {
    IHS_Session *s=IHS_TestSessionCreate();
    IHS_TimerTaskStopImmediate(s->retransmission.timer);
    IHS_SessionChannelControl *c=(void *)s->channels[1];
    IHS_TimerTaskStopImmediate(c->feedbackTimer);
    return s;
}
static void boundaries(void) {
    for (unsigned paused=0;paused<2;++paused) for (unsigned wrap=0;wrap<2;++wrap)
        for (int delta=-1;delta<=1;++delta) {
            IHS_Session *s=new_session();
            uint32_t now=wrap ? 65536u : 0x40000000u;
            uint32_t age=(paused ? 180u : 15u)*65536u+delta;
            s->state.connectionState=IHS_SessionConnectionStateHandshaking;
            s->lastPacketReceived=now-age;
            s->connectionPaused=s->clientPaused=paused;
            atomic_store(&s->pauseRequested,paused);
            IHS_SessionActivityUpdate(s,now);
            assert(s->inactivityClosing==(delta>=0));
            IHS_SessionDestroy(s);
        }
}
static void pause_resume(void) {
    IHS_Session *s=new_session();
    s->state.connectionState=IHS_SessionConnectionStateConnected;
    uint32_t now=0x40000000;
    s->lastPacketReceived=now;
    const IHS_SessionChannelClass cls={.instanceSize=sizeof(IHS_SessionChannel)};
    s->channels[s->numChannels++]=IHS_SessionChannelCreate(&cls,s,IHS_SessionChannelTypeDataAudio,7,NULL);
    s->channels[s->numChannels++]=IHS_SessionChannelCreate(&cls,s,IHS_SessionChannelTypeDataVideo,8,NULL);
    IHS_SessionSetPaused(s,true);
    IHS_SessionActivityUpdate(s,now);
    assert(pauses==1 && resumes==0 && s->connectionPaused);
    IHS_SessionActivityUpdate(s,now+15u*65536u);
    assert(pauses==1 && !s->inactivityClosing);
    IHS_SessionSetPaused(s,false);
    IHS_SessionActivityUpdate(s,now+179u*65536u);
    assert(resumes==1 && !s->connectionPaused && !s->inactivityClosing);
    assert(s->lastPacketReceived==now+179u*65536u);
    for (unsigned id=7;id<=8;++id) {
        IHS_QueueItem *q=IHS_QueuePoll(s->sendQueue);
        assert(q && !q->reliable && q->packet.header.channelId==id);
        assert(q->packet.header.type==IHS_SessionPacketTypeUnreliable);
        assert(q->packet.body.size==1 && *IHS_BufferPointer(&q->packet.body)==2);
        IHS_SessionPacketClear(&q->packet,true); IHS_QueueItemFree((void *)q);
    }
    assert(!IHS_QueuePoll(s->sendQueue));
    IHS_SessionActivityUpdate(s,now+180u*65536u);
    assert(resumes==1 && !s->inactivityClosing);
    IHS_SessionDestroy(s);
}
static void receive_gate(void) {
    IHS_Session *s=new_session();
    s->state.connectionState=IHS_SessionConnectionStateHandshaking;
    s->state.connectionId=0x11; s->state.hostConnectionId=0x22;
    for (unsigned which=0;which<5;++which) {
        IHS_SessionPacket p={0};
        p.header.type=which==3 ? IHS_SessionPacketTypeUnconnected : IHS_SessionPacketTypeACK;
        p.header.channelId=which==3 ? 0 : 2;
        p.header.srcConnectionId=which==1 ? 0x23 : 0x22;
        p.header.dstConnectionId=which==2 ? 0x12 : 0x11;
        p.header.hasCrc=true;
        IHS_SessionPacketBodyInitialize(&p.body,true);
        IHS_BufferAppendUInt32LE(&p.body,0);
        IHS_SessionPacketPopulateBuffer(&p);
        IHS_Buffer b; IHS_BufferTransferOwnership(&p.body,&b); IHS_BufferExtendSize(&b);
        if (which==4) IHS_BufferPointer(&b)[b.size-1]^=1;
        s->lastPacketReceived=123;
        s->base.callbacks.received(&s->base,&s->info.address,&b);
        assert((s->lastPacketReceived!=123)==(which==0));
        IHS_BufferClear(&b,true);
        IHS_SessionPacketClear(&p,true);
    }
    IHS_SessionDestroy(s);
}
typedef struct idle_provider { atomic_uint polls; } idle_provider;
static bool idle_send(void *context,const uint8_t *bytes,size_t size) {
    (void)context;(void)bytes;(void)size;return true;
}
static int idle_receive(void *context,uint8_t *bytes,size_t size,uint32_t wait_us) {
    (void)bytes;(void)size;(void)wait_us;
    idle_provider *p=context;
    usleep(1000);
    return atomic_fetch_add(&p->polls,1)<2000 ? 0 : -1;
}
static void owner_loop(void) {
    IHS_Session *s=new_session();
    idle_provider p={0};
    const IHS_DatagramTransport provider={.send=idle_send,.receive=idle_receive};
    assert(IHS_SessionSetDatagramTransport(s,&provider,&p));
    s->lastPacketReceived=IHS_SessionPacketTimestamp()-15u*65536u;
    assert(IHS_SessionConnect(s));
    IHS_SessionThreadedJoin(s);
    assert(s->inactivityClosing && !s->sendThread && s->base.interrupted);
    assert(atomic_load(&p.polls)<2000);
    IHS_SessionDestroy(s);
}
int main(void) {
    IHS_Init();
    boundaries(); pause_resume(); receive_gate(); owner_loop();
    IHS_Quit(); puts("PASS connection inactivity and pause/resume");
}
