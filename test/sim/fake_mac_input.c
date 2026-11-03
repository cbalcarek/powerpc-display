/* fake Mac: known pairing, one frame, INPUT_ON, 2.5 s of a 440 Hz tone; checks INPUT messages */
#include "g4codec.h"
#include "g4auth.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <math.h>
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
int main(void){
    uint8_t hid[16]; memset(hid,0x42,16);
    int fd=socket(AF_INET,SOCK_STREAM,0),type,i; struct sockaddr_in a; memset(&a,0,sizeof a);
    a.sin_family=AF_INET;a.sin_port=htons(5960);a.sin_addr.s_addr=htonl(0x7f000001);
    for(i=0;i<50&&connect(fd,(struct sockaddr*)&a,sizeof a);i++){usleep(100000);close(fd);fd=socket(AF_INET,SOCK_STREAM,0);}
    static uint8_t b[1<<16]; uint8_t cid[16],nc[16],nh[16],key[32],ikey[32],auth[68]; uint32_t n;
    if(readmsg(fd,&type,b,&n)||type!=G4_MSG_HELLO){puts("SIM: no hello");return 1;}
    printf("SIM: HELLO flags 0x%x (input %s, audio %s)\n",b[11],b[11]&G4_HELLO_FLAG_INPUT?"yes":"no",b[11]&G4_HELLO_FLAG_AUDIO?"yes":"no");
    int W=g4_get16(b+6),H=g4_get16(b+8),depth=b[10];
    memcpy(cid,b+16,16);memcpy(nc,b+32,16);
    g4_random(nh,16); memset(auth,0,4); memcpy(auth+4,hid,16); memcpy(auth+20,nh,16);
    FILE*f=fopen("hostsim.key","rb"); if(!f||fread(key,1,32,f)!=32){puts("SIM: no key");return 1;} fclose(f);
    auth[0]=G4_AUTH_KNOWN; g4_auth_tag(auth+36,key,'H',cid,hid,nc,nh);
    msg(fd,G4_MSG_AUTH,auth,68);
    if(readmsg(fd,&type,b,&n)||type!=G4_MSG_AUTH_OK){puts("SIM: auth failed");return 1;}
    g4_auth_tag(ikey,key,'I',cid,hid,nc,nh);
    uint8_t ping[4]={0,0,0,1}; msg(fd,G4_MSG_PING,ping,4);
    g4_encoder*e=g4enc_create(W,H,depth); uint8_t*bgra=calloc((size_t)W*H,4);
    g4enc_ingest_bgra(e,bgra,W*4); const uint8_t*out; g4_enc_stats st; size_t len=g4enc_encode(e,1,&out,&st); write(fd,out,len);
    uint8_t on=1; msg(fd,G4_MSG_INPUT_ON,&on,1);
    /* sound: 20 ms packets of 48 kHz stereo */
    g4_adpcm_state as[2]={{0,0},{0,0}}; int16_t pcm[960*2]; uint8_t pkt[G4_AUDIO_HDR+2*(4+480)]; uint32_t aseq=0; long sample=0;
    double t0=now(), next=t0, lp=t0; uint32_t lastseq=0; int good=0, bad=0, events=0, esc=0;
    fcntl(fd,F_SETFL,O_NONBLOCK);
    while(now()-t0<4000){
        if(now()>=next && now()-t0<2500){
            for(i=0;i<960;i++,sample++){ int16_t v=(int16_t)(8000*sin(2*M_PI*440*sample/48000.0)); pcm[2*i]=v; pcm[2*i+1]=v; }
            g4_put32(pkt,++aseq); g4_put32(pkt+4,48000); g4_put16(pkt+8,960); pkt[10]=2; pkt[11]=G4_AUDIO_ADPCM;
            size_t bl=g4_adpcm_encode(as,pcm,960,2,pkt+G4_AUDIO_HDR);
            fcntl(fd,F_SETFL,0); msg(fd,G4_MSG_AUDIO,pkt,(uint32_t)(G4_AUDIO_HDR+bl)); fcntl(fd,F_SETFL,O_NONBLOCK);
            next+=20;
        }
        if(now()-lp>1000){ping[3]++; fcntl(fd,F_SETFL,0); msg(fd,G4_MSG_PING,ping,4); fcntl(fd,F_SETFL,O_NONBLOCK); lp=now();}
        uint8_t h[8]; ssize_t r=recv(fd,h,8,MSG_PEEK);
        if(r==8){ fcntl(fd,F_SETFL,0); readmsg(fd,&type,b,&n); fcntl(fd,F_SETFL,O_NONBLOCK);
            if(type==G4_MSG_INPUT){
                uint8_t mac[16]; uint32_t body=n-16, seq=g4_get32(b); g4_input_mac(mac,ikey,b,body);
                if(!g4_equal(mac,b+body,16)||seq<=lastseq){bad++;continue;} good++; lastseq=seq;
                for(uint32_t o=4;o<body;o+=8,events++){ uint8_t*ev=b+o;
                    if(ev[0]==G4_IN_KEY){ printf("SIM: %.0f ms key %u %s\n",now()-t0,ev[1],ev[2]?"down":"up"); if(ev[1]==53) esc++; }
                    else if(ev[0]==G4_IN_MOUSE) printf("SIM: %.0f ms mouse %d,%d buttons %u\n",now()-t0,(int16_t)g4_get16(ev+2),(int16_t)g4_get16(ev+4),ev[1]);
                    else if(ev[0]==G4_IN_SCROLL) printf("SIM: scroll %d\n",(int16_t)g4_get16(ev+2));
                }
            }
        } else if(r==0) break;
        usleep(1000);
    }
    printf("SIM: %d signed input messages ok, %d bad, %d events, Escape sent %d times\n",good,bad,events,esc);
    return bad||!good||esc ? 1 : 0;
}
