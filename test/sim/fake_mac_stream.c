#include <fcntl.h>
#include "g4auth.h"
/* fake Apple Silicon Mac: handshake (pair / known / wrongkey), ping, one frame, then silence */
#include "g4codec.h"
#include "g4auth.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
static double now(void){struct timeval t;gettimeofday(&t,0);return t.tv_sec*1e3+t.tv_usec/1e3;}
static int rd(int fd, void *b, size_t n){uint8_t*p=b;while(n){ssize_t r=read(fd,p,n);if(r<=0)return -1;p+=r;n-=r;}return 0;}
static int msg(int fd,int type,const void*p,uint32_t n){uint8_t h[8];g4_put_header(h,type,n);return write(fd,h,8)!=8||(n&&write(fd,p,n)!=(ssize_t)n);}
static int readmsg(int fd,int*type,uint8_t*b,uint32_t*n){uint8_t h[8];if(rd(fd,h,8))return -1;*type=h[0];*n=g4_get32(h+4);return *n?rd(fd,b,*n):0;}
int main(int argc,char**argv){
    const char*mode=argv[1]; uint8_t hid[16]; memset(hid,0x42,16);
    int fd=socket(AF_INET,SOCK_STREAM,0),type,i; struct sockaddr_in a; memset(&a,0,sizeof a);
    a.sin_family=AF_INET;a.sin_port=htons(5960);a.sin_addr.s_addr=htonl(0x7f000001);
    for(i=0;i<50&&connect(fd,(struct sockaddr*)&a,sizeof a);i++){usleep(100000);close(fd);fd=socket(AF_INET,SOCK_STREAM,0);}
    uint8_t b[4096],cid[16],nc[16],cpub[32],nh[16],key[32],tag[32],auth[68]; uint32_t n;
    if(readmsg(fd,&type,b,&n)||type!=G4_MSG_HELLO){puts("SIM: no hello");return 1;}
    printf("SIM: HELLO %u bytes, version %u, %ux%u, depth %u, flags 0x%x\n",n,g4_get16(b+4),g4_get16(b+6),g4_get16(b+8),b[10],b[11]);
    if(n!=G4_HELLO_V2_LEN||!(b[11]&G4_HELLO_FLAG_AUTH)){puts("SIM: not v2");return 1;}
    int W=g4_get16(b+6),H=g4_get16(b+8),depth=b[10];
    memcpy(cid,b+16,16);memcpy(nc,b+32,16);memcpy(cpub,b+48,32);
    g4_random(nh,16); memset(auth,0,4); memcpy(auth+4,hid,16); memcpy(auth+20,nh,16);
    if(!strcmp(mode,"pair")){
        uint8_t hpub[32],hpriv[32]; g4_dh_keypair(hpub,hpriv);
        auth[0]=G4_AUTH_PAIR; memcpy(auth+36,hpub,32);
        g4_pair_key(key,hpriv,cpub,cpub,hpub,cid,hid);
        printf("SIM: pairing code on the 'M1': %06u\n",(unsigned)g4_pair_code(key));
    } else {
        FILE*f=fopen("hostsim.key","rb"); if(!f||fread(key,1,32,f)!=32){puts("SIM: no key");return 1;} fclose(f);
        if(!strcmp(mode,"wrongkey")) key[0]^=1;
        auth[0]=G4_AUTH_KNOWN; g4_auth_tag(auth+36,key,'H',cid,hid,nc,nh);
    }
    msg(fd,G4_MSG_AUTH,auth,68);
    if(readmsg(fd,&type,b,&n)){puts("SIM: closed during auth");return 1;}
    if(type==G4_MSG_AUTH_FAIL){printf("SIM: AUTH_FAIL reason %u\n",g4_get32(b));return 2;}
    g4_auth_tag(tag,key,'C',cid,hid,nc,nh);
    if(type!=G4_MSG_AUTH_OK||n!=32||!g4_equal(tag,b,32)){puts("SIM: bad AUTH_OK");return 1;}
    puts("SIM: client proved the key (AUTH_OK verified)");
    if(!strcmp(mode,"pair")){FILE*f=fopen("hostsim.key","wb");fwrite(key,1,32,f);fclose(f);}
    if(getenv("UPDATEZIP")){  /* expect INFO, offer an update, serve it */
        FILE*f=fopen(getenv("UPDATEZIP"),"rb"); static uint8_t z[1<<20]; size_t zn=fread(z,1,sizeof z,f); fclose(f);
        if(readmsg(fd,&type,b,&n)||type!=G4_MSG_INFO){puts("SIM: no INFO");return 1;} b[n]=0; printf("SIM: client version %s\n",b);
        uint8_t off[4+64+8]; g4_put32(off,(uint32_t)zn); g4_sha512(off+4,z,zn); memcpy(off+68,"9.9",3); msg(fd,G4_MSG_UPDATE_OFFER,off,71);
        if(readmsg(fd,&type,b,&n)||type!=G4_MSG_UPDATE_REQ){printf("SIM: no UPDATE_REQ (%d)\n",type);return 1;} puts("SIM: client asked for the update");
        static uint8_t ch[4+65536]; size_t o; for(o=0;o<zn;o+=65536){ size_t k=zn-o<65536?zn-o:65536; g4_put32(ch,(uint32_t)o); memcpy(ch+4,z+o,k); msg(fd,G4_MSG_UPDATE_DATA,ch,(uint32_t)(4+k)); }
        printf("SIM: sent %zu bytes\n",zn);
    }
    uint8_t ping[4]={0,0,0,7}; msg(fd,G4_MSG_PING,ping,4);
    g4_encoder*e=g4enc_create(W,H,depth); uint8_t*bgra=malloc((size_t)W*H*4);
    for(i=0;i<W*H*4;i++)bgra[i]=(uint8_t)(i*7); g4enc_ingest_bgra(e,bgra,W*4);
    const uint8_t*out; g4_enc_stats st; size_t len=g4enc_encode(e,1,&out,&st); write(fd,out,len);
    int pong=0,ack=0; double t0=now();
    while((!pong||!ack)&&now()-t0<5000){ if(readmsg(fd,&type,b,&n))break; if(type==G4_MSG_PONG)pong=1; if(type==G4_MSG_ACK&&g4_get32(b)==1)ack=1; }
    printf("SIM: PONG %s, frame ACK %s\n",pong?"yes":"NO",ack?"yes":"NO");
    if(getenv("STREAM")){ /* 12 s of frames (~20/s) and pings, then silence */
        uint32_t id=1; double ts=now(), lp=0; int k;
        fcntl(fd,F_SETFL,O_NONBLOCK);
        while(now()-ts<12000){
            for(k=0;k<W*40*4;k++) bgra[(size_t)(id%(H/40))*W*40*4+k]^=0x5a;
            g4enc_ingest_bgra(e,bgra,W*4); len=g4enc_encode(e,++id,&out,&st);
            fcntl(fd,F_SETFL,0); if(len) write(fd,out,len);
            if(now()-lp>1000){ping[3]++; msg(fd,G4_MSG_PING,ping,4); lp=now();}
            fcntl(fd,F_SETFL,O_NONBLOCK); while(read(fd,b,sizeof b)>0); usleep(50000);
        }
        fcntl(fd,F_SETFL,0);
        if(getenv("HOSTIMG")){FILE*f=fopen(getenv("HOSTIMG"),"wb");fwrite(bgra,1,(size_t)W*H*4,f);fclose(f);}
    }
    if(getenv("SLEEPMSG")){ msg(fd,11,0,0); puts("SIM: sent SLEEP"); t0=now(); while(read(fd,b,sizeof b)>0); printf("SIM: client closed %.1f s after SLEEP\n",(now()-t0)/1000); return 0; }
    puts("SIM: going silent (like a pulled cable)");
    t0=now(); while(read(fd,b,sizeof b)>0); printf("SIM: client hung up after %.1f s of silence\n",(now()-t0)/1000);
    return (pong&&ack)?0:1;
}
