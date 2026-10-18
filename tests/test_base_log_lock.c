/* A log callback snapshot must complete while another thread owns Base.
 * This detects the sendLock -> Base logging edge without needing a deadlock. */
#include "base.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static atomic_uint calls;
static void logged(IHS_LogLevel level,const char *tag,const char *message) {
    assert(level==IHS_LogLevelDebug && !strcmp(tag,"lock-test") && !strcmp(message,"value=7"));
    atomic_fetch_add(&calls,1);
}
typedef struct Probe {
    IHS_Base *base;
    IHS_Mutex *gate;
    IHS_Cond *changed;
    bool done, replace;
} Probe;
static void run(void *context) {
    Probe *p=context;
    if(p->replace)IHS_BaseSetLogFunction(p->base,logged);
    IHS_BaseLog(p->base,IHS_LogLevelDebug,"lock-test","value=%d",7);
    IHS_MutexLock(p->gate);p->done=true;IHS_CondSignal(p->changed);IHS_MutexUnlock(p->gate);
}
int main(void) {
    IHS_Init();const uint8_t secret[32]={0};
    IHS_ClientConfig config={.deviceId=1,.secretKey=secret};IHS_Base base;
    IHS_BaseInit(&base,&config,NULL,false);
    bool blocked=false;
    for(unsigned mode=0;mode<3;++mode) {
        IHS_BaseSetLogFunction(&base,mode==1?logged:NULL);
        Probe p={.base=&base,.gate=IHS_MutexCreate(),.changed=IHS_CondCreate(),.replace=mode==2};
        assert(p.gate && p.changed);
        IHS_BaseLock(&base);
        IHS_Thread *worker=IHS_ThreadCreate(run,"LogLockTest",&p);assert(worker);
        IHS_MutexLock(p.gate);
        while(!p.done)if(!IHS_CondWaitTimeout(p.changed,p.gate,2000))break;
        bool completed=p.done;IHS_MutexUnlock(p.gate);
        /* Always let an old, blocked implementation cleanly join before fail. */
        IHS_BaseUnlock(&base);IHS_ThreadJoin(worker);
        IHS_CondDestroy(p.changed);IHS_MutexDestroy(p.gate);
        if(!completed) {
            fprintf(stderr,"Log snapshot blocked by Base ownership (mode=%u)\n",mode);
            blocked=true;break;
        }
    }
    if(!blocked)assert(atomic_load(&calls)==2);
    IHS_BaseSetLogFunction(&base,NULL);IHS_BaseDestroy(&base);IHS_Quit();
    if(blocked)return 1;
    puts("Base log snapshot/setter complete independently of Base ownership");return 0;
}
