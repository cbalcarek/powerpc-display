#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
typedef uint32_t CGDirectDisplayID; typedef int CGError; enum { kCGErrorSuccess=0 };
typedef struct { double width, height; } CGSize;
CGDirectDisplayID CGMainDisplayID(void); size_t CGDisplayPixelsWide(CGDirectDisplayID); size_t CGDisplayPixelsHigh(CGDirectDisplayID);
size_t CGDisplayBitsPerPixel(CGDirectDisplayID); size_t CGDisplayBytesPerRow(CGDirectDisplayID); void *CGDisplayBaseAddress(CGDirectDisplayID);
CGError CGDisplayCapture(CGDirectDisplayID); CGError CGDisplayHideCursor(CGDirectDisplayID); CGError CGDisplayShowCursor(CGDirectDisplayID); CGError CGReleaseAllDisplays(void);
unsigned int CGDisplayIDToOpenGLDisplayMask(CGDirectDisplayID); CGSize CGDisplayScreenSize(CGDirectDisplayID);
typedef void *CGColorSpaceRef, *CGDataProviderRef, *CGImageRef, *CFURLRef, *CGImageDestinationRef, *CFStringRef; typedef long CFIndex; typedef unsigned char UInt8;
CGColorSpaceRef CGColorSpaceCreateDeviceRGB(void); CGDataProviderRef CGDataProviderCreateWithData(void*,const void*,size_t,void*);
enum { kCGImageAlphaNoneSkipFirst=1, kCGBitmapByteOrderDefault=0, kCGRenderingIntentDefault=0 };
CGImageRef CGImageCreate(size_t,size_t,size_t,size_t,size_t,CGColorSpaceRef,int,CGDataProviderRef,const double*,bool,int);
CFURLRef CFURLCreateFromFileSystemRepresentation(void*,const UInt8*,CFIndex,bool); CGImageDestinationRef CGImageDestinationCreateWithURL(CFURLRef,CFStringRef,size_t,void*);
#define CFSTR(x) ((CFStringRef)x)
void CGImageDestinationAddImage(CGImageDestinationRef,CGImageRef,void*); bool CGImageDestinationFinalize(CGImageDestinationRef); void CFRelease(void*);
void CGImageRelease(CGImageRef); void CGDataProviderRelease(CGDataProviderRef); void CGColorSpaceRelease(CGColorSpaceRef);
typedef struct { double x, y; } CGPoint;
static inline CGPoint CGPointMake(double x, double y) { CGPoint p = { x, y }; return p; }
CGError CGDisplayMoveCursorToPoint(CGDirectDisplayID d, CGPoint p);
typedef unsigned int boolean_t; typedef const void *CFDictionaryRef;
CFDictionaryRef CGDisplayBestModeForParameters(CGDirectDisplayID, size_t, size_t, size_t, boolean_t *);
CGError CGDisplaySwitchToMode(CGDirectDisplayID, CFDictionaryRef);
typedef void *CGEventRef; typedef int CGEventSourceStateID; typedef int CGMouseButton;
enum { kCGEventSourceStateHIDSystemState = 1, kCGMouseButtonLeft = 0, kCGMouseButtonRight = 1, kCGMouseButtonCenter = 2 };
CGEventRef CGEventCreate(void *); CGPoint CGEventGetLocation(CGEventRef); bool CGEventSourceButtonState(CGEventSourceStateID, CGMouseButton);
