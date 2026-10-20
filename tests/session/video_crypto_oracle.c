/* Real mbedTLS-backed crypto and the production consumer/tracker/reset actor.
 * SDK/player callbacks and direct Q input inherit the declared mode boundary. */
#include "test_session.h"
#include "session/session_pri.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
static size_t cryptoKeyLen;
static IHS_Session *cryptoSession(void) {
    IHS_Session *s = IHS_TestSessionCreate();
    assert(s && cryptoKeyLen <= sizeof(s->info.sessionKey));
    for (size_t n = 0; n < cryptoKeyLen; ++n) s->info.sessionKey[n] = n;
    s->info.sessionKeyLen = cryptoKeyLen;
    return s;
}
#define IHS_TestSessionCreate cryptoSession
#define main mode_vectors_main
#include "video_content_mode_oracle.c"
#undef main
#undef IHS_TestSessionCreate
static size_t unhex(const char *raw, uint8_t *out) {
    size_t len = strlen(raw)/2;
    assert(strlen(raw)==len*2);
    for (size_t n=0;n<len;++n) {
        unsigned v; assert(sscanf(raw+2*n,"%2x",&v)==1); out[n]=v;
    }
    return len;
}
static void hexbytes(const uint8_t *b,size_t len) {
    for(size_t n=0;n<len;++n)printf("%02x",b[n]);
}
int main(int argc, char **argv) {
    assert(argc>=2);
    if (!strcmp(argv[1],"D")) {
        assert(argc==7);
        size_t keylen=strtoul(argv[2],NULL,10),capacity=strtoul(argv[5],NULL,10);
        size_t len=strlen(argv[4])/2,room=len>capacity?len:capacity;
        if(!room)room=1;
        assert(room<=65536 && (keylen==16 || keylen==32));
        uint8_t iv[16],key[32]; assert(strlen(argv[3])==32 && unhex(argv[3],iv)==16);
        for(size_t n=0;n<keylen;++n)key[n]=n;
        uint8_t *input=malloc(room+16);assert(input);memset(input,165,room+16);
        assert(unhex(argv[4],input)==len);
        bool inplace=atoi(argv[6]);
        uint8_t *out=inplace?input:malloc(room+16);assert(out);
        if(!inplace)memset(out,165,room+16);
        size_t outlen=capacity;
        int ret=IHS_CryptoSymmetricDecryptWithIV(input,len,iv,16,key,keylen,out,&outlen);
        printf("{\"ok\":%s,\"out_len\":%zu,\"out\":\"",ret==0?"true":"false",outlen);
        hexbytes(out,room);printf("\",\"input_after\":\"");hexbytes(input,len);
        printf("\",\"guard\":\"");hexbytes(out+room,16);puts("\"}");
        if(!inplace)free(out);
        free(input);
        return 0;
    }
    assert(argc==5 && !strcmp(argv[1],"V"));
    cryptoKeyLen=strtoul(argv[2],NULL,10);
    assert(cryptoKeyLen==16 || cryptoKeyLen==32);
    char *args[]={argv[0],argv[3],atoi(argv[3])?"0":"-1","1",argv[4]};
    return mode_vectors_main(5,args);
}
