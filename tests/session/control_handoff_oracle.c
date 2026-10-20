/* Real assembled-frame admission/decryption, owned FIFO and drain. Terminal
 * handlers are observed adapters except peer SetConfig and config99. The
 * independent official binary supplies all golden outputs and cipher inputs. */
#include "ihslib.h"
#include "ihs_buffer.h"
#include "ihs_buffer_ext.h"
#include "session/channels/ch_control.h"
#include "session/session_pri.h"
#include "test_session.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static IHS_Session *session;
static const char *phase = "receive";
static char *events[1024];
static unsigned eventCount;
static void hex(FILE *f, const uint8_t *data, size_t size) {
    for (size_t i=0;i<size;++i) fprintf(f,"%02x",data[i]);
}
static void value(FILE *f, EStreamControlMessage type, const IHS_Buffer *body,
                    const IHS_SessionPacketHeader *h, const char *where) {
    fprintf(f,"{\"type\":%u,\"body\":\"",(unsigned)type);
    hex(f,IHS_BufferPointer(body),body->size);
    fprintf(f,"\",\"sent\":%u,\"receive\":%u",h->sendTimestamp,h->receiveTimestamp);
    if(where)fprintf(f,",\"phase\":\"%s\"",where);
    fputc('}',f);
}
void __real_IHS_SessionChannelControlOnMessageReceived(IHS_SessionChannel *,EStreamControlMessage,
                                                        IHS_Buffer *,const IHS_SessionPacketHeader *);
void __wrap_IHS_SessionChannelControlOnMessageReceived(IHS_SessionChannel *ch,EStreamControlMessage type,
                                                        IHS_Buffer *body,const IHS_SessionPacketHeader *header) {
    if(type==2||type==3||type==4||type==7||type==81||type==99||type==106) {
        assert(eventCount<1024);
        size_t size=0;FILE *f=open_memstream(&events[eventCount++],&size);assert(f);
        value(f,type,body,header,phase);assert(!fclose(f));
    }
    if(type==4||type==99)
        __real_IHS_SessionChannelControlOnMessageReceived(ch,type,body,header);
}
static void protobuf(const ProtobufCMessage *message) {
    if(!message)return;
    size_t size=protobuf_c_message_get_packed_size(message);
    uint8_t *data=malloc(size?size:1);assert(data);
    assert(protobuf_c_message_pack(message,data)==size);hex(stdout,data,size);free(data);
}
static void report(void) {
    IHS_SessionChannelControl *control=(void *)session->channels[1];
    printf("{\"state\":%d,\"receive_sequence\":%llu,\"queue\":[",session->state.connectionState,
        (unsigned long long)control->recvEncryptSequence);
    IHS_MutexLock(control->incomingLock);
    unsigned count=0;
    for(IHS_IncomingControl *item=control->incomingHead;item;item=item->next) {
        if(count++)putchar(',');
        value(stdout,item->type,&item->body,&item->header,NULL);
    }
    assert(count==control->incomingCount);IHS_MutexUnlock(control->incomingLock);
    printf("],\"events\":[");
    for(unsigned i=0;i<eventCount;++i){printf("%s%s",i?",":"",events[i]);free(events[i]);}
    eventCount=0;printf("],\"negotiated\":\"");
    protobuf((void *)session->negotiatedConfig);printf("\",\"client_config\":\"");
    protobuf((void *)session->clientConfig);puts("\"}");
}
int main(int argc,char **argv) {
    assert(argc==2);IHS_Init();session=IHS_TestSessionCreate();
    IHS_TimerTaskStopImmediate(session->retransmission.timer);
    session->state.connectionState=atoi(argv[1]);
    session->negotiatedConfig=cnegotiated_config__unpack(NULL,0,NULL);assert(session->negotiatedConfig);
    char line[8192],raw[4096];unsigned sent,received,state;
    while(fgets(line,sizeof(line),stdin)) {
        if(sscanf(line,"R %u %u %4095s",&sent,&received,raw)==3) {
            phase="receive";
            IHS_SessionFrame frame={.header={.type=IHS_SessionPacketTypeReliable,.channelId=1,
                .sendTimestamp=sent,.receiveTimestamp=received,.receiveBegin=received}};
            IHS_BufferInit(&frame.body,1024,0);
            for(size_t i=0;strcmp(raw,"-") && raw[i];i+=2) {unsigned b;assert(raw[i+1]&&sscanf(raw+i,"%2x",&b)==1);
                IHS_BufferAppendUInt8(&frame.body,b);}
            IHS_SessionChannelControlReceiveFrame(session->channels[1],&frame);
            IHS_SessionFrameClear(&frame,true);
        } else if(sscanf(line,"S %u",&state)==1) session->state.connectionState=state;
        else if(sscanf(line,"T %u",&state)==1) {
            assert(state==2||state==3);atomic_store(&session->transport.shutdown,state-1);
        }
        else {assert(line[0]=='D');phase="drain";IHS_SessionChannelControlDrainIncoming(session->channels[1]);}
        report();
    }
    IHS_SessionDestroy(session);assert(eventCount==0);IHS_Quit();return 0;
}
