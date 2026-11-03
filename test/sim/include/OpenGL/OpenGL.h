typedef struct _CGLContextObject *CGLContextObj; typedef struct _CGLPixelFormatObject *CGLPixelFormatObj; typedef struct _CGLRendererInfoObject *CGLRendererInfoObj;
typedef int CGLPixelFormatAttribute, CGLError, CGLRendererProperty, CGLContextParameter; typedef int GLint_;
enum { kCGLNoError=0, kCGLPFAFullScreen, kCGLPFADisplayMask, kCGLPFADoubleBuffer, kCGLPFAAccelerated, kCGLPFANoRecovery, kCGLPFAColorSize, kCGLCPSwapInterval, kCGLRPAccelerated, kCGLRPVideoMemory };
CGLError CGLChoosePixelFormat(const CGLPixelFormatAttribute*, CGLPixelFormatObj*, int*); CGLError CGLCreateContext(CGLPixelFormatObj, CGLContextObj, CGLContextObj*);
CGLError CGLDestroyPixelFormat(CGLPixelFormatObj); CGLError CGLSetCurrentContext(CGLContextObj); CGLError CGLSetFullScreen(CGLContextObj);
CGLError CGLSetParameter(CGLContextObj, CGLContextParameter, const int*); CGLError CGLClearDrawable(CGLContextObj); CGLError CGLDestroyContext(CGLContextObj); CGLError CGLFlushDrawable(CGLContextObj);
CGLError CGLQueryRendererInfo(unsigned int, CGLRendererInfoObj*, int*); CGLError CGLDescribeRenderer(CGLRendererInfoObj, int, CGLRendererProperty, int*); CGLError CGLDestroyRendererInfo(CGLRendererInfoObj);
