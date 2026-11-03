/* do-nothing stand-ins for the Mac APIs, so client.c's engine runs on Linux */
#include <string.h>
#include <stdlib.h>
#include "OpenGL/OpenGL.h"
#include "OpenGL/gl.h"
#include "OpenGL/glext.h"
#include "ApplicationServices/ApplicationServices.h"
#include "Carbon/Carbon.h"
#include "dns_sd.h"
#include "sys/sysctl.h"

void glViewport(GLint a,GLint b,GLsizei c,GLsizei d){} void glMatrixMode(GLenum m){} void glLoadIdentity(void){}
void glOrtho(double a,double b,double c,double d,double e,double f){} void glDisable(GLenum e){} void glEnable(GLenum e){}
void glTexEnvi(GLenum a,GLenum b,GLint c){} void glClearColor(float a,float b,float c,float d){} 
 void glTexParameteri(GLenum a,GLenum b,GLint c){} 


void glDeleteTextures(GLsizei n,const GLuint*t){} void glFinish(void){} void glBegin(GLenum m){} void glEnd(void){}
void glTexCoord2i(GLint a,GLint b){} void glVertex2i(GLint a,GLint b){} void glBlendFunc(GLenum a,GLenum b){}
GLenum glGetError(void){return 0;} 
void glTextureRangeAPPLE(GLenum a,GLsizei b,const void*c){} void glFinishObjectAPPLE(GLenum a,GLint b){}


CGLError CGLDestroyPixelFormat(CGLPixelFormatObj a){return 0;} CGLError CGLSetCurrentContext(CGLContextObj a){return 0;}
 CGLError CGLSetParameter(CGLContextObj a,CGLContextParameter b,const int*c){return 0;}
CGLError CGLClearDrawable(CGLContextObj a){return 0;} CGLError CGLDestroyContext(CGLContextObj a){return 0;} CGLError CGLFlushDrawable(CGLContextObj a){return 0;}
CGLError CGLQueryRendererInfo(unsigned int a,CGLRendererInfoObj*b,int*c){return 1;}
CGLError CGLDescribeRenderer(CGLRendererInfoObj a,int b,CGLRendererProperty c,int*d){return 1;} CGLError CGLDestroyRendererInfo(CGLRendererInfoObj a){return 0;}
CGDirectDisplayID CGMainDisplayID(void){return 1;} size_t CGDisplayPixelsWide(CGDirectDisplayID d){return 640;} size_t CGDisplayPixelsHigh(CGDirectDisplayID d){return 480;}
  
 CGError CGDisplayHideCursor(CGDirectDisplayID d){return 0;} CGError CGDisplayShowCursor(CGDirectDisplayID d){return 0;} 
unsigned int CGDisplayIDToOpenGLDisplayMask(CGDirectDisplayID d){return 1;} CGSize CGDisplayScreenSize(CGDirectDisplayID d){CGSize s={300,200};return s;}
CGColorSpaceRef CGColorSpaceCreateDeviceRGB(void){return 0;} CGDataProviderRef CGDataProviderCreateWithData(void*a,const void*b,size_t c,void*d){return 0;}
CGImageRef CGImageCreate(size_t a,size_t b,size_t c,size_t d,size_t e,CGColorSpaceRef f,int g,CGDataProviderRef h,const double*i,bool j,int k){return 0;}
CFURLRef CFURLCreateFromFileSystemRepresentation(void*a,const UInt8*b,CFIndex c,bool d){return 0;} CGImageDestinationRef CGImageDestinationCreateWithURL(CFURLRef a,CFStringRef b,size_t c,void*d){return 0;}
void CGImageDestinationAddImage(CGImageDestinationRef a,CGImageRef b,void*c){} bool CGImageDestinationFinalize(CGImageDestinationRef a){return 0;} void CFRelease(void*a){}
void CGImageRelease(CGImageRef a){} void CGDataProviderRelease(CGDataProviderRef a){} void CGColorSpaceRelease(CGColorSpaceRef a){}
void UpdateSystemActivity(int a){}
DNSServiceErrorType DNSServiceRegister(DNSServiceRef*a,DNSServiceFlags b,unsigned c,const char*d,const char*e,const char*f,const char*g,unsigned short h,unsigned short i,const void*j,DNSServiceRegisterReply k,void*l){return -1;}
void DNSServiceRefDeallocate(DNSServiceRef a){} int DNSServiceRefSockFD(DNSServiceRef a){return -1;} DNSServiceErrorType DNSServiceProcessResult(DNSServiceRef a){return -1;}
DNSServiceErrorType DNSServiceResolve(DNSServiceRef*a,DNSServiceFlags b,uint32_t c,const char*d,const char*e,const char*f,DNSServiceResolveReply g,void*h){return -1;}
int sysctlbyname(const char*a, void*b, size_t*c, void*d, size_t e){return -1;}
CGError CGDisplayMoveCursorToPoint(CGDirectDisplayID d, CGPoint p){return 0;}

void glTexCoord2f(float a,float b){}



/* ---- fake Mac for render tests: MODE=gl2d (Rage 128-like GPU) or MODE=raw ---- */
#include <stdio.h>
static int mode_gl2d(void){const char*m=getenv("MODE");return m&&(!strcmp(m,"gl2d")||!strcmp(m,"rage"));}
static int mode_raw(void){const char*m=getenv("MODE");return m&&(!strcmp(m,"raw")||!strcmp(m,"rage"));}
static int rowlen=0, tw=0, th=0, tfmt=0, bound=0, screen_id=-1; static unsigned char *tex=0;
static unsigned char *vram=0; static int vbpp=-1, captured=0;
static int scr_bpp(void){ if(vbpp<0) vbpp=getenv("SCREEN16")?16:32; return vbpp; }
void glGenTextures(GLsizei n,GLuint*t){ static GLuint next=1; int i; for(i=0;i<n;i++) t[i]=next++; }
const GLubyte *glGetString(GLenum e){ return (const GLubyte*)(e==GL_EXTENSIONS?"GL_APPLE_packed_pixels GL_APPLE_client_storage":"fake"); }
void glPixelStorei(GLenum a,GLint b){ if(a==GL_UNPACK_ROW_LENGTH) rowlen=b; }
void glGetIntegerv(GLenum a,GLint*b){ *b = a==GL_MAX_TEXTURE_SIZE ? 1024 : 0; }
void glBindTexture(GLenum a,GLuint b){ bound=b; }
static int bpp_of(GLenum t){ return t==GL_UNSIGNED_SHORT_1_5_5_5_REV?2:4; }
void glTexImage2D(GLenum t,GLint l,GLint i,GLsizei w,GLsizei h,GLint b,GLenum f,GLenum ty,const void*p){
    if(w < 640) return;                        /* the cursor texture */
    screen_id = bound;
    if(t!=GL_TEXTURE_2D){ fprintf(stderr,"FAKE: rect texture used on a GPU without it!\n"); exit(9); }
    tw=w; th=h; tfmt=ty; free(tex); tex=calloc((size_t)w*h, 4);
    if(p) fprintf(stderr,"FAKE: TexImage with data\n");
}
void glTexSubImage2D(GLenum t,GLint l,GLint x,GLint y,GLsizei w,GLsizei h,GLenum f,GLenum ty,const void*p){
    int bp=bpp_of(ty), rl=rowlen?rowlen:w, r;
    if(bound!=screen_id) return;
    if(x+w>tw||y+h>th){ fprintf(stderr,"FAKE: subimage out of bounds\n"); exit(9); }
    for(r=0;r<h;r++) memcpy(tex+((size_t)(y+r)*tw+x)*bp, (const unsigned char*)p+(size_t)r*rl*bp, (size_t)w*bp);
}
CGLError CGLChoosePixelFormat(const CGLPixelFormatAttribute*a,CGLPixelFormatObj*b,int*c){ if(!mode_gl2d()) return 1; *b=(CGLPixelFormatObj)1; *c=1; return 0; }
CGLError CGLCreateContext(CGLPixelFormatObj a,CGLContextObj b,CGLContextObj*c){ *c=(CGLContextObj)1; return 0; }
CGLError CGLSetFullScreen(CGLContextObj a){ return 0; }
size_t CGDisplayBitsPerPixel(CGDirectDisplayID d){ return (size_t)scr_bpp(); }
size_t CGDisplayBytesPerRow(CGDirectDisplayID d){ return (size_t)640*scr_bpp()/8 + 64; }
void *CGDisplayBaseAddress(CGDirectDisplayID d){ if(!mode_raw()) return 0; if(!vram) vram=calloc(1,(size_t)(640*4+64)*480); return vram; }
CGError CGDisplayCapture(CGDirectDisplayID d){ captured=1; return 0; }
CFDictionaryRef CGDisplayBestModeForParameters(CGDirectDisplayID d,size_t b,size_t w,size_t h,boolean_t*e){ if(e)*e=getenv("SWITCH")?1:0; return (CFDictionaryRef)1; }
CGError CGDisplaySwitchToMode(CGDirectDisplayID d,CFDictionaryRef m){ if(!getenv("SWITCH")) return 1; vbpp=16; return 0; }
CGError CGReleaseAllDisplays(void){
    const char*out=getenv("DUMP");
    if(captured && out){ FILE*f=fopen(out,"wb");
        if(vram && mode_raw()){ int y,b=vbpp/8; fprintf(f,"V%d\n",b); for(y=0;y<480;y++) fwrite(vram+(size_t)y*(640*b+64),1,(size_t)640*b,f); }
        else if(tex){ int y; fprintf(f,"T%d\n",bpp_of(tfmt)); for(y=0;y<480;y++) fwrite(tex+(size_t)y*tw*bpp_of(tfmt),1,(size_t)640*bpp_of(tfmt),f); }
        else if(vram){ int y,b=vbpp/8; fprintf(f,"V%d\n",b); for(y=0;y<480;y++) fwrite(vram+(size_t)y*(640*b+64),1,(size_t)640*b,f); }
        fclose(f); }
    captured=0; vbpp=-1; return 0; }

/* ---- input and sound fakes ---- */
#include <pthread.h>
#include <sys/time.h>
#include <unistd.h>
#include <AudioUnit/AudioUnit.h>
#include "g4client.h"
static double t0ms = 0;
static double tms(void){ struct timeval tv; gettimeofday(&tv,0); double t=tv.tv_sec*1e3+tv.tv_usec/1e3; if(!t0ms) t0ms=t; return t-t0ms; }
/* script: from 1.0 s hold Command (55) + 'a' (0) for 150 ms, then 'b' (11); also Command-Escape at 2.2 s (Escape must not be sent) */
void GetKeys(KeyMap km){ unsigned char *k=(unsigned char*)km; double t=tms(); memset(km,0,sizeof(KeyMap)); if(getenv("NOKEYS")) return;
    #define SET(c) k[(c)>>3] |= (unsigned char)(1<<((c)&7))
    if(t>1000&&t<1150){ SET(55); SET(0); }
    if(t>1300&&t<1400){ SET(11); }
    if(t>2200&&t<2250){ SET(55); SET(53); }
}
CGEventRef CGEventCreate(void *s){ return (CGEventRef)1; }
CGPoint CGEventGetLocation(CGEventRef e){ double t=tms(); CGPoint p={100,100}; if(t>1500&&t<1700){ p.x=100+(t-1500)/2; p.y=120; } else if(t>=1700){ p.x=200; p.y=120; } return p; }
bool CGEventSourceButtonState(CGEventSourceStateID s, CGMouseButton b){ double t=tms(); return b==0 && t>1800 && t<1900; }
/* audio: a fake output that pulls 512 frames every ~10.7 ms and counts what it got */
static AURenderCallbackStruct fcb; static AudioStreamBasicDescription ffmt; static volatile int frun=0; static pthread_t fth;
static long played_nonzero=0, played_total=0;
static void *fake_out(void*a){ static int16_t buf[512*2]; AudioBufferList l; while(frun){ l.mNumberBuffers=1; l.mBuffers[0].mData=buf; l.mBuffers[0].mDataByteSize=sizeof buf;
    fcb.inputProc(fcb.inputProcRefCon,0,0,0,512,&l); int i; for(i=0;i<512*(int)ffmt.mChannelsPerFrame;i++){ played_total++; if(buf[i]) played_nonzero++; }
    usleep(10667); } return 0; }
Component FindNextComponent(Component c, ComponentDescription *d){ return (Component)1; }
OSStatus OpenAComponent(Component c, ComponentInstance *i){ *i=(ComponentInstance)1; return 0; }
OSStatus CloseComponent(ComponentInstance i){ return 0; }
OSStatus AudioUnitSetProperty(AudioUnit u, UInt32 p, UInt32 s, UInt32 e, const void *d, UInt32 n){ if(p==kAudioUnitProperty_SetRenderCallback) fcb=*(const AURenderCallbackStruct*)d; if(p==kAudioUnitProperty_StreamFormat) ffmt=*(const AudioStreamBasicDescription*)d; return 0; }
OSStatus AudioUnitInitialize(AudioUnit u){ return 0; } OSStatus AudioUnitUninitialize(AudioUnit u){ return 0; }
OSStatus AudioOutputUnitStart(AudioUnit u){ frun=1; pthread_create(&fth,0,fake_out,0); fprintf(stderr,"FAKE: audio out %.0f Hz %u ch\n",ffmt.mSampleRate,(unsigned)ffmt.mChannelsPerFrame); return 0; }
OSStatus AudioOutputUnitStop(AudioUnit u){ frun=0; pthread_join(fth,0); fprintf(stderr,"FAKE: played %ld samples, %ld non-silent\n",played_total,played_nonzero); return 0; }
int client_main(int, char**);
int main(int argc, char **argv){ tms(); g4c_set_app_mode(1); return client_main(argc, argv); }
