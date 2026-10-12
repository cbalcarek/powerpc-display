/*
 * PowerPC Display — the PowerPC Mac side (Mac OS X 10.4 Tiger; G3, G4 or G5).
 *
 * Receives the Apple Silicon Mac's screen and shows it full screen. When the
 * other Mac disconnects, this Mac's own desktop comes back. Command+Escape
 * quits (in the app: pauses and returns to the PowerPC Display window).
 *
 * Performance notes:
 *   - Pixels arrive in this Mac's native big-endian format: no conversion.
 *   - The decode buffer is the OpenGL texture's backing store (Apple client
 *     storage + texture range), so changed rows go to the GPU by DMA.
 *   - Only the rows that changed are re-uploaded; a fence keeps the CPU from
 *     overwriting memory the GPU is still reading.
 *   - A tile cache lets content seen before (menus closing, switching apps
 *     back) arrive as 12-byte references instead of pixels. Its size follows
 *     the installed RAM.
 *   - The colour depth is picked for the machine: 16-bit on a G3 (no AltiVec,
 *     slower bus) or with little video memory, 32-bit otherwise.
 *   - Without usable OpenGL (old ATI Rage chips, very large screens, or an
 *     emulator) it draws straight into the framebuffer with 8-byte stores,
 *     which is safe for video memory.
 *   - Advertised with Bonjour as _g4display._tcp, so the other Mac finds it
 *     without IP addresses (works over a direct cable).
 *   - Keeps the display from sleeping while it is showing the other Mac.
 *
 * Build: double-click "Build PowerPC Display.command" (needs Xcode 2.5). It
 *        makes one app with code tuned for G3, G4 (7400 and 7450) and G5;
 *        the system picks the right one at launch.
 * Run:   ./ppcdisplay  (-16/-32: colour depth, -c N: cache tiles, -v: stats,
 *                       -t: test mode without drawing, for emulators)
 */
#include "g4codec.h"
#include "g4client.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <ApplicationServices/ApplicationServices.h>
#include <Carbon/Carbon.h>

#include <arpa/inet.h>
#include <dns_sd.h>
#ifndef DNSSD_API
#define DNSSD_API     /* older dns_sd.h headers don't define it */
#endif
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <unistd.h>

static int  opt_depth   = 0;    /* 0 = automatic */
static int  opt_verbose = 0;
static int  opt_port    = G4_PORT;
static int  opt_slots   = -1;   /* -1 = automatic (from RAM) */
static int  opt_test    = 0;   /* -t: no OpenGL, decode only, save PNG snapshots */
static volatile int quit_requested = 0;
static int app_mode = 0;   /* set by the Tiger app (app.m) */
static void report(int state, const char *text);

/* ------------------------------------------------------------------ */

static double now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

static int read_full(int fd, void *buf, size_t n)
{
    uint8_t *p = (uint8_t *)buf;
    while (n) {
        ssize_t r = read(fd, p, n);
        if (r == 0) return -1;
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        p += r; n -= (size_t)r;
    }
    return 0;
}

static int write_full(int fd, const void *buf, size_t n)
{
    const uint8_t *p = (const uint8_t *)buf;
    while (n) {
        ssize_t r = write(fd, p, n);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        p += r; n -= (size_t)r;
    }
    return 0;
}

static int send_u32_msg(int fd, int type, uint32_t v)
{
    uint8_t m[G4_HDR_SIZE + 4];
    g4_put_header(m, type, 4);
    g4_put32(m + G4_HDR_SIZE, v);
    return write_full(fd, m, sizeof m);
}

/* Command (55) + Escape (53) held down */
static int quit_keys_down(void)
{
    KeyMap km;
    const unsigned char *k = (const unsigned char *)km;
    GetKeys(km);
    return ((k[53 >> 3] >> (53 & 7)) & 1) && ((k[55 >> 3] >> (55 & 7)) & 1);
}

static int has_ext(const char *name)
{
    const char *exts = (const char *)glGetString(GL_EXTENSIONS);
    size_t n = strlen(name);
    while (exts && (exts = strstr(exts, name)) != NULL) {
        if (exts[n] == ' ' || exts[n] == '\0') return 1;
        exts += n;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* what kind of PowerPC Mac this is */

static struct {
    int done;
    int gen;            /* 3 = G3, 4 = G4, 5 = G5 */
    int altivec;
    int subtype;
    double mem_mb;
    double vram_mb;     /* largest accelerated renderer on the main display, 0 = unknown */
} mach;

static void probe_machine(void)
{
    int v = 0;
    size_t n;
    unsigned long long mem = 0;

    if (mach.done) return;
    mach.done = 1;
    n = sizeof v;
    if (sysctlbyname("hw.optional.altivec", &v, &n, NULL, 0) == 0) mach.altivec = v != 0;
    else { n = sizeof v; if (sysctlbyname("hw.vectorunit", &v, &n, NULL, 0) == 0) mach.altivec = v != 0; }
    n = sizeof v; v = 0;
    if (sysctlbyname("hw.cpusubtype", &v, &n, NULL, 0) == 0) mach.subtype = v;
    switch (mach.subtype) {
    case 9:   mach.gen = 3; break;                 /* CPU_SUBTYPE_POWERPC_750  */
    case 10: case 11: mach.gen = 4; break;         /* 7400 / 7450              */
    case 100: mach.gen = 5; break;                 /* 970                      */
    default:  mach.gen = mach.altivec ? 4 : 3; break;
    }
    n = sizeof mem;
    if (sysctlbyname("hw.memsize", &mem, &n, NULL, 0) == 0) {
        mach.mem_mb = (double)mem / (1024.0 * 1024.0);
    }

    {
        CGLRendererInfoObj ri = NULL;
        GLint nr = 0, i;
        if (CGLQueryRendererInfo(CGDisplayIDToOpenGLDisplayMask(CGMainDisplayID()), &ri, &nr) == kCGLNoError && ri) {
            for (i = 0; i < nr; i++) {
                GLint acc = 0, vram = 0;
                CGLDescribeRenderer(ri, i, kCGLRPAccelerated, &acc);
                CGLDescribeRenderer(ri, i, kCGLRPVideoMemory, &vram);
                if (acc && vram / 1048576.0 > mach.vram_mb) mach.vram_mb = vram / 1048576.0;
            }
            CGLDestroyRendererInfo(ri);
        }
    }
}

/* colour depth to use when the user hasn't chosen one */
static int auto_depth(int W, int H)
{
    double need_mb = (double)W * H * 4 * 3 / 1048576.0 + 4;   /* 2 screen buffers + texture */
    probe_machine();
    if ((int)CGDisplayBitsPerPixel(CGMainDisplayID()) == 16) return 16;   /* screen is in Thousands */
    if (mach.gen == 3) return 16;                                         /* G3: half the memory traffic */
    if (mach.vram_mb > 0 && mach.vram_mb < need_mb) return 16;            /* not enough VRAM for 32-bit */
    return 32;
}

/* tile cache size that fits comfortably in RAM */
static int auto_slots(void)
{
    probe_machine();
    if (mach.mem_mb <= 0)   return G4_DEFAULT_SLOTS;
    if (mach.mem_mb <= 256) return 512;
    if (mach.mem_mb <= 512) return 1024;
    if (mach.mem_mb <= 1024) return 2048;
    return 4096;
}

int g4c_auto_depth(void)
{
    CGDirectDisplayID d = CGMainDisplayID();
    return auto_depth((int)CGDisplayPixelsWide(d), (int)CGDisplayPixelsHigh(d));
}

const char *g4c_machine_summary(void)
{
    static char buf[160];
    const char *cpu;
    probe_machine();
    switch (mach.subtype) {
    case 9:   cpu = "PowerPC G3"; break;
    case 10:  cpu = "PowerPC G4 (7400)"; break;
    case 11:  cpu = "PowerPC G4 (7450)"; break;
    case 100: cpu = "PowerPC G5"; break;
    default:  cpu = mach.gen == 3 ? "PowerPC G3" : "PowerPC G4"; break;
    }
    if (mach.vram_mb > 0)
        snprintf(buf, sizeof buf, "%s, %.0f MB RAM, %.0f MB video memory", cpu, mach.mem_mb, mach.vram_mb);
    else
        snprintf(buf, sizeof buf, "%s, %.0f MB RAM", cpu, mach.mem_mb);
    return buf;
}

/* ------------------------------------------------------------------ */

typedef struct {
    int fd;
    int W, H, depth, bpp;
    CGDirectDisplayID display;
    CGLContextObj ctx;
    GLuint fbtex, curtex;
    GLenum fmt_type;
    uint8_t *fb;
    size_t fbsize;
    g4_decoder dec;
    int fence;
    int gl_tried, got_frame;   /* screen is only taken over once a frame arrives */
    int render;                /* 0 not yet, 1 OpenGL, 2 direct framebuffer (fallback) */
    uint8_t *raw_base; size_t raw_rowbytes;
    int oc_x, oc_y, oc_w, oc_h; /* where the cursor was last painted (direct mode) */
    uint8_t *cur_px;           /* cursor image, A,R,G,B premultiplied */

    int cur_x, cur_y, cur_vis, cur_w, cur_h, cur_hx, cur_hy;

    uint8_t *msg; size_t msg_cap;

    /* stats */
    double stat_t0;
    unsigned stat_frames, stat_draws;
    double stat_bytes, stat_decode_ms;
    double stat_wait_ms, stat_upload_ms, stat_flush_ms, stat_upload_kb;
} session;

/* Call fn for each run of consecutive changed bands (rows y0..y1). Only the
   bands that changed are uploaded: a menu-bar clock and a line of typing at
   the bottom no longer mean re-sending the whole screen in between. */
typedef void (*band_fn)(session *s, int y0, int y1);
static void for_each_dirty_run(session *s, band_fn fn)
{
    const g4_decoder *d = &s->dec;
    int b = 0;
    while (b < d->nbands) {
        int b0;
        if (!d->dirty_bands[b]) { b++; continue; }
        b0 = b;
        while (b < d->nbands && d->dirty_bands[b]) b++;
        {
            int y0 = b0 * G4_DIRTY_BAND, y1 = b * G4_DIRTY_BAND;
            if (y1 > s->H) y1 = s->H;
            fn(s, y0, y1);
        }
    }
}

static void mark_rows(session *s, int y, int h)
{
    int b;
    if (y < 0) { h += y; y = 0; }
    if (y + h > s->H) h = s->H - y;
    for (b = y / G4_DIRTY_BAND; h > 0 && b * G4_DIRTY_BAND < y + h; b++) s->dec.dirty_bands[b] = 1;
}

static int gl_setup(session *s, int allow_soft)
{
    CGLPixelFormatAttribute attrs[] = {
        kCGLPFAFullScreen,
        kCGLPFADisplayMask, (CGLPixelFormatAttribute)CGDisplayIDToOpenGLDisplayMask(s->display),
        kCGLPFADoubleBuffer,
        kCGLPFAAccelerated,
        kCGLPFANoRecovery,
        kCGLPFAColorSize, (CGLPixelFormatAttribute)24,
        (CGLPixelFormatAttribute)0
    };
    CGLPixelFormatObj pf = NULL;
    GLint npf = 0, one = 1;

    if (CGDisplayCapture(s->display) != kCGErrorSuccess) { fprintf(stderr, "cannot capture display\n"); return -1; }
    CGDisplayHideCursor(s->display);

    if (CGLChoosePixelFormat(attrs, &pf, &npf) != kCGLNoError || !pf) {
        if (!allow_soft) { fprintf(stderr, "no accelerated OpenGL renderer\n"); return -1; }
        /* no accelerated GPU (e.g. an emulated Mac in UTM/QEMU): fall back to
           Apple's software renderer — slow, but enough to see that it works */
        CGLPixelFormatAttribute soft[] = {
            kCGLPFAFullScreen,
            kCGLPFADisplayMask, (CGLPixelFormatAttribute)CGDisplayIDToOpenGLDisplayMask(s->display),
            kCGLPFADoubleBuffer,
            kCGLPFAColorSize, (CGLPixelFormatAttribute)24,
            (CGLPixelFormatAttribute)0
        };
        pf = NULL;
        if (CGLChoosePixelFormat(soft, &pf, &npf) != kCGLNoError || !pf) {
            fprintf(stderr, "no OpenGL full-screen renderer on this Mac; try ./g4display -t (test mode)\n");
            return -1;
        }
        fprintf(stderr, "no accelerated OpenGL: using the software renderer (slow)\n");
    }
    if (CGLCreateContext(pf, NULL, &s->ctx) != kCGLNoError) { CGLDestroyPixelFormat(pf); return -1; }
    CGLDestroyPixelFormat(pf);
    CGLSetCurrentContext(s->ctx);
    if (CGLSetFullScreen(s->ctx) != kCGLNoError) { fprintf(stderr, "cannot go full screen\n"); return -1; }
    CGLSetParameter(s->ctx, kCGLCPSwapInterval, &one);   /* sync to refresh: no tearing */

    glViewport(0, 0, s->W, s->H);
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glOrtho(0, s->W, s->H, 0, -1, 1);                     /* y down, 1 unit = 1 pixel */
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glDisable(GL_DEPTH_TEST); glDisable(GL_DITHER); glDisable(GL_LIGHTING);
    glEnable(GL_TEXTURE_RECTANGLE_EXT);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glClearColor(0, 0, 0, 1);

    s->fence = has_ext("GL_APPLE_fence");
    if (opt_verbose)
        fprintf(stderr, "GL: %s / %s, fence=%d\n", glGetString(GL_RENDERER), glGetString(GL_VERSION), s->fence);

    /* the screen is one rectangle texture: the GPU must support that size
       (e.g. a Radeon 9600 tops out at 2048 wide, too small for a 30" display) */
    {
        GLint maxrect = 0;
        if (!has_ext("GL_EXT_texture_rectangle") && !has_ext("GL_ARB_texture_rectangle") &&
            !has_ext("GL_NV_texture_rectangle")) {
            fprintf(stderr, "GPU has no rectangle textures\n");
            return -1;
        }
        glGetIntegerv(GL_MAX_RECTANGLE_TEXTURE_SIZE_EXT, &maxrect);
        if (maxrect < s->W || maxrect < s->H) {
            fprintf(stderr, "screen %dx%d is larger than the GPU's textures (%d)\n", s->W, s->H, (int)maxrect);
            return -1;
        }
    }

    /* framebuffer texture backed directly by our memory (zero-copy uploads) */
    s->fmt_type = s->depth == 16 ? GL_UNSIGNED_SHORT_1_5_5_5_REV : GL_UNSIGNED_INT_8_8_8_8_REV;
    glGenTextures(1, &s->fbtex);
    glBindTexture(GL_TEXTURE_RECTANGLE_EXT, s->fbtex);
    if (has_ext("GL_APPLE_texture_range"))
        glTextureRangeAPPLE(GL_TEXTURE_RECTANGLE_EXT, (GLsizei)s->fbsize, s->fb);
    /* CACHED, not SHARED: the texture is redrawn on every cursor move, and
       SHARED would make the GPU re-read it from system memory each time */
    glTexParameteri(GL_TEXTURE_RECTANGLE_EXT, GL_TEXTURE_STORAGE_HINT_APPLE, GL_STORAGE_CACHED_APPLE);
    glPixelStorei(GL_UNPACK_CLIENT_STORAGE_APPLE, GL_TRUE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexParameteri(GL_TEXTURE_RECTANGLE_EXT, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_RECTANGLE_EXT, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_RECTANGLE_EXT, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_RECTANGLE_EXT, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    while (glGetError() != GL_NO_ERROR) {}
    glTexImage2D(GL_TEXTURE_RECTANGLE_EXT, 0, s->depth == 16 ? GL_RGB5_A1 : GL_RGBA,
                 s->W, s->H, 0, GL_BGRA, s->fmt_type, s->fb);
    if (glGetError() != GL_NO_ERROR) {
        fprintf(stderr, "GPU refused the %dx%d screen texture\n", s->W, s->H);
        return -1;
    }

    /* small ordinary texture for the cursor */
    glPixelStorei(GL_UNPACK_CLIENT_STORAGE_APPLE, GL_FALSE);
    glGenTextures(1, &s->curtex);
    glBindTexture(GL_TEXTURE_RECTANGLE_EXT, s->curtex);
    glTexParameteri(GL_TEXTURE_RECTANGLE_EXT, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_RECTANGLE_EXT, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glPixelStorei(GL_UNPACK_CLIENT_STORAGE_APPLE, GL_TRUE);
    return 0;
}

static void gl_teardown(session *s)
{
    if (s->ctx) {
        glDeleteTextures(1, &s->fbtex);
        glDeleteTextures(1, &s->curtex);
        CGLSetCurrentContext(NULL);
        CGLClearDrawable(s->ctx);
        CGLDestroyContext(s->ctx);
        s->ctx = NULL;
    }
    CGDisplayShowCursor(s->display);
    CGReleaseAllDisplays();
}

/* GPU may still be reading the framebuffer memory: wait before overwriting it */
static void wait_fb_idle(session *s)
{
    double t0 = now_ms();
    if (s->fence) glFinishObjectAPPLE(GL_TEXTURE, s->fbtex);
    else glFinish();
    s->stat_wait_ms += now_ms() - t0;
}

static void upload_rows(session *s, int y0, int y1)
{
    glTexSubImage2D(GL_TEXTURE_RECTANGLE_EXT, 0, 0, y0, s->W, y1 - y0, GL_BGRA, s->fmt_type,
                    s->fb + (size_t)y0 * s->dec.stride);
    s->stat_upload_kb += (double)(y1 - y0) * s->dec.stride / 1024.0;
}

static void draw(session *s)
{
    double t0 = now_ms(), t1;
    glBindTexture(GL_TEXTURE_RECTANGLE_EXT, s->fbtex);
    if (s->dec.dirty_y1 > s->dec.dirty_y0) {
        glPixelStorei(GL_UNPACK_CLIENT_STORAGE_APPLE, GL_TRUE);
        for_each_dirty_run(s, upload_rows);
        g4dec_reset_dirty(&s->dec);
    }
    glDisable(GL_BLEND);
    glBegin(GL_QUADS);
    glTexCoord2i(0, 0);         glVertex2i(0, 0);
    glTexCoord2i(s->W, 0);      glVertex2i(s->W, 0);
    glTexCoord2i(s->W, s->H);   glVertex2i(s->W, s->H);
    glTexCoord2i(0, s->H);      glVertex2i(0, s->H);
    glEnd();

    if (s->cur_vis && s->cur_w > 0) {
        int x = s->cur_x - s->cur_hx, y = s->cur_y - s->cur_hy;
        glBindTexture(GL_TEXTURE_RECTANGLE_EXT, s->curtex);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);         /* premultiplied */
        glBegin(GL_QUADS);
        glTexCoord2i(0, 0);               glVertex2i(x, y);
        glTexCoord2i(s->cur_w, 0);        glVertex2i(x + s->cur_w, y);
        glTexCoord2i(s->cur_w, s->cur_h); glVertex2i(x + s->cur_w, y + s->cur_h);
        glTexCoord2i(0, s->cur_h);        glVertex2i(x, y + s->cur_h);
        glEnd();
    }
    t1 = now_ms();
    s->stat_upload_ms += t1 - t0;
    CGLFlushDrawable(s->ctx);          /* may wait for the display's refresh */
    s->stat_flush_ms += now_ms() - t1;
    s->stat_draws++;
}

static void upload_cursor(session *s)
{
    if (!s->cur_px || s->cur_w <= 0) return;
    glBindTexture(GL_TEXTURE_RECTANGLE_EXT, s->curtex);
    glPixelStorei(GL_UNPACK_CLIENT_STORAGE_APPLE, GL_FALSE);
    glTexImage2D(GL_TEXTURE_RECTANGLE_EXT, 0, GL_RGBA, (GLsizei)s->cur_w, (GLsizei)s->cur_h, 0,
                 GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, s->cur_px);
    glPixelStorei(GL_UNPACK_CLIENT_STORAGE_APPLE, GL_TRUE);
}

/* ---- fallback renderer: no usable OpenGL (e.g. an emulated Mac), so write
   straight into the captured display's framebuffer. The wire formats are
   the framebuffer formats (big-endian xRGB8888 / x1555), so rows are memcpy'd. */
/* Copy into video memory. The system memcpy uses dcbz, which on uncached
   video memory traps on every cache line (very slow on real hardware), so
   use plain 8-byte floating-point loads/stores, which copy bits exactly. */
static void vram_copy(uint8_t *dst, const uint8_t *src, size_t n)
{
    if ((((unsigned long)dst | (unsigned long)src) & 7) == 0) {
        double *d = (double *)dst;
        const double *p = (const double *)src;
        size_t k = n >> 3, i = 0;
        for (; i + 4 <= k; i += 4) {
            double a = p[i], b = p[i + 1], c = p[i + 2], e = p[i + 3];
            d[i] = a; d[i + 1] = b; d[i + 2] = c; d[i + 3] = e;
        }
        for (; i < k; i++) d[i] = p[i];
        dst += k << 3; src += k << 3; n &= 7;
    } else if ((((unsigned long)dst | (unsigned long)src) & 3) == 0) {
        uint32_t *d = (uint32_t *)dst;
        const uint32_t *p = (const uint32_t *)src;
        size_t k = n >> 2, i;
        for (i = 0; i < k; i++) d[i] = p[i];
        dst += k << 2; src += k << 2; n &= 3;
    }
    while (n--) *dst++ = *src++;
}

static int raw_setup(session *s)
{
    CGDirectDisplayID d = s->display;
    if (CGDisplayCapture(d) != kCGErrorSuccess) return -1;
    CGDisplayHideCursor(d);
    s->raw_base = (uint8_t *)CGDisplayBaseAddress(d);
    s->raw_rowbytes = CGDisplayBytesPerRow(d);
    if (!s->raw_base || (int)CGDisplayBitsPerPixel(d) != s->depth ||
        (int)CGDisplayPixelsWide(d) != s->W || (int)CGDisplayPixelsHigh(d) != s->H) {
        fprintf(stderr, "direct framebuffer not usable (%d bits per pixel)\n", (int)CGDisplayBitsPerPixel(d));
        s->raw_base = NULL;
        return -1;
    }
    s->oc_w = 0;
    s->dec.dirty_y0 = 0; s->dec.dirty_y1 = s->H;    /* paint everything once */
    mark_rows(s, 0, s->H);
    fprintf(stderr, "drawing directly to the framebuffer (no suitable OpenGL)\n");
    return 0;
}

static void raw_copy_rows(session *s, int y0, int y1)
{
    size_t rowb = (size_t)s->W * s->bpp;
    int y;
    for (y = y0; y < y1; y++)
        vram_copy(s->raw_base + (size_t)y * s->raw_rowbytes, s->fb + (size_t)y * s->dec.stride, rowb);
    s->stat_upload_kb += (double)(y1 - y0) * rowb / 1024.0;
}

static void raw_draw(session *s)
{
    int nx = s->cur_x - s->cur_hx, ny = s->cur_y - s->cur_hy;
    int show = s->cur_vis && s->cur_px && s->cur_w > 0;
    double t0 = now_ms();

    /* rows under the old and new cursor are refreshed from the clean frame */
    if (s->oc_w) mark_rows(s, s->oc_y, s->oc_h);
    if (show) mark_rows(s, ny, s->cur_h);
    for_each_dirty_run(s, raw_copy_rows);
    g4dec_reset_dirty(&s->dec);
    s->oc_w = 0;

    if (show) {
        int cx, cy;
        for (cy = 0; cy < s->cur_h; cy++) {
            int sy = ny + cy;
            if (sy < 0 || sy >= s->H) continue;
            for (cx = 0; cx < s->cur_w; cx++) {
                int sx = nx + cx;
                const uint8_t *src = s->cur_px + ((size_t)cy * s->cur_w + cx) * 4;
                unsigned a = src[0];
                if (sx < 0 || sx >= s->W || a == 0) continue;
                if (s->bpp == 4) {
                    uint8_t *d = s->raw_base + (size_t)sy * s->raw_rowbytes + (size_t)sx * 4;
                    d[1] = (uint8_t)(src[1] + d[1] * (255 - a) / 255);
                    d[2] = (uint8_t)(src[2] + d[2] * (255 - a) / 255);
                    d[3] = (uint8_t)(src[3] + d[3] * (255 - a) / 255);
                } else if (a >= 128) {
                    uint8_t *d = s->raw_base + (size_t)sy * s->raw_rowbytes + (size_t)sx * 2;
                    unsigned v = 0x8000u | ((unsigned)(src[1] >> 3) << 10) | ((unsigned)(src[2] >> 3) << 5) | (src[3] >> 3);
                    d[0] = (uint8_t)(v >> 8); d[1] = (uint8_t)v;
                }
            }
        }
        s->oc_x = nx; s->oc_y = ny; s->oc_w = s->cur_w; s->oc_h = s->cur_h;
    }
    s->stat_upload_ms += now_ms() - t0;
    s->stat_draws++;
}

static void set_cursor_shape(session *s, const uint8_t *p, size_t len)
{
    unsigned w, h;
    if (len < 8) return;
    w = g4_get16(p); h = g4_get16(p + 2);
    if (w == 0 || h == 0 || w > 256 || h > 256 || len < 8 + (size_t)w * h * 4) return;
    s->cur_w = (int)w; s->cur_h = (int)h;
    s->cur_hx = (int)g4_get16(p + 4); s->cur_hy = (int)g4_get16(p + 6);
    {
        uint8_t *np = (uint8_t *)realloc(s->cur_px, (size_t)w * h * 4);
        if (!np) { s->cur_w = 0; return; }
        memcpy(np, p + 8, (size_t)w * h * 4);
        s->cur_px = np;
    }
    if (s->render == 1) upload_cursor(s);
}

/* ---- test mode: write the decoded framebuffer as a PNG (ImageIO, 10.4+) ---- */
static const char *snapshot_path(void)
{
    static char path[1024];
    const char *home = getenv("HOME");
    snprintf(path, sizeof path, "%s/Desktop/ppcdisplay-test.png", home ? home : "/tmp");
    return path;
}

static void save_snapshot(session *s)
{
    const char *path = snapshot_path();
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGDataProviderRef dp = CGDataProviderCreateWithData(NULL, s->fb, s->fbsize, NULL);
    /* wire formats are big-endian xRGB 8888 / x1555, i.e. "skip first" */
    CGImageRef img = CGImageCreate((size_t)s->W, (size_t)s->H,
                                   s->depth == 16 ? 5 : 8, s->depth == 16 ? 16 : 32,
                                   s->dec.stride, cs, kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrderDefault,
                                   dp, NULL, false, kCGRenderingIntentDefault);
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, (CFIndex)strlen(path), false);
    CGImageDestinationRef dst = url ? CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL) : NULL;
    if (img && dst) {
        CGImageDestinationAddImage(dst, img, NULL);
        if (CGImageDestinationFinalize(dst)) fprintf(stderr, "snapshot saved: %s\n", path);
    }
    if (dst) CFRelease(dst);
    if (url) CFRelease(url);
    if (img) CGImageRelease(img);
    CGDataProviderRelease(dp);
    CGColorSpaceRelease(cs);
}

/* read and handle exactly one message. returns 0, or -1 on disconnect/error */
static int handle_message(session *s, int *need_draw, uint32_t *frame_to_ack)
{
    uint8_t hdr[G4_HDR_SIZE];
    uint32_t len;
    int type;

    if (read_full(s->fd, hdr, sizeof hdr)) return -1;
    type = hdr[0];
    len = g4_get32(hdr + 4);
    if (len > G4_MAX_PAYLOAD) { fprintf(stderr, "oversized message\n"); return -1; }
    if (len > s->msg_cap) {
        uint8_t *nb = (uint8_t *)realloc(s->msg, len);
        if (!nb) return -1;
        s->msg = nb; s->msg_cap = len;
    }
    if (len && read_full(s->fd, s->msg, len)) return -1;
    s->stat_bytes += len + G4_HDR_SIZE;

    switch (type) {
    case G4_MSG_FRAME: {
        double t0 = now_ms();
        uint32_t fid = 0;
        int rc;
        if (s->render == 1) wait_fb_idle(s);
        s->got_frame = 1;
        rc = g4dec_frame(&s->dec, s->msg, len, &fid);
        if (rc) { fprintf(stderr, "bad frame (%d)\n", rc); return -1; }
        s->stat_decode_ms += now_ms() - t0;
        s->stat_frames++;
        *frame_to_ack = fid;
        *need_draw = 1;
        break;
    }
    case G4_MSG_CURSOR_POS:
        if (len >= 5) {
            int x = (int16_t)g4_get16(s->msg), y = (int16_t)g4_get16(s->msg + 2), v = s->msg[4];
            if (x != s->cur_x || y != s->cur_y || v != s->cur_vis) {
                s->cur_x = x; s->cur_y = y; s->cur_vis = v;
                *need_draw = 1;
            }
        }
        break;
    case G4_MSG_CURSOR_SHAPE:
        set_cursor_shape(s, s->msg, len);
        *need_draw = 1;
        break;
    case G4_MSG_PING:
        if (len >= 4 && send_u32_msg(s->fd, G4_MSG_PONG, g4_get32(s->msg))) return -1;
        break;
    default:
        break;   /* unknown: ignore (payload already consumed) */
    }
    return 0;
}

static void run_session(int fd)
{
    session s;
    uint8_t hello[G4_HDR_SIZE + 16];
    int one = 1, buf = 1 << 20;
    double last_keys = 0, last_awake = 0;

    memset(&s, 0, sizeof s);
    s.fd = fd;
    s.display = CGMainDisplayID();
    s.W = (int)CGDisplayPixelsWide(s.display);
    s.H = (int)CGDisplayPixelsHigh(s.display);
    s.depth = opt_depth ? opt_depth : auto_depth(s.W, s.H);
    s.bpp = s.depth == 16 ? 2 : 4;
    s.fbsize = (size_t)s.W * s.H * s.bpp;
    s.cur_vis = 0;

    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf, sizeof buf);

    g4_put_header(hello, G4_MSG_HELLO, 16);
    memcpy(hello + G4_HDR_SIZE, "G4DP", 4);
    g4_put16(hello + G4_HDR_SIZE + 4, G4_PROTO_VERSION);
    g4_put16(hello + G4_HDR_SIZE + 6, (uint32_t)s.W);
    g4_put16(hello + G4_HDR_SIZE + 8, (uint32_t)s.H);
    hello[G4_HDR_SIZE + 10] = (uint8_t)s.depth;
    /* 4:2:2 video regions: decoded on the CPU into the normal framebuffer,
       so every drawing path supports them (32-bit only) */
    hello[G4_HDR_SIZE + 11] = s.depth == 32 ? G4_HELLO_FLAG_YUV : 0;

    s.fb = (uint8_t *)valloc(s.fbsize);                       /* page aligned for DMA */
    if (!s.fb) return;
    memset(s.fb, 0, s.fbsize);
    if (g4dec_init(&s.dec, s.fb, s.W, s.H, s.depth)) { free(s.fb); return; }
    {
        int slots = opt_slots >= 0 ? opt_slots : auto_slots();
        if (slots > 0 && g4dec_enable_cache(&s.dec, slots))
            fprintf(stderr, "not enough memory for %d cache tiles; cache off\n", slots);
    }
    g4_put16(hello + G4_HDR_SIZE + 12, (uint32_t)s.dec.cache_slots);
    {
        /* physical width in mm, so the other Mac can pick sensible scaling */
        CGSize mm = CGDisplayScreenSize(s.display);
        unsigned wmm = (mm.width > 0 && mm.width < 65535) ? (unsigned)(mm.width + 0.5) : 0;
        g4_put16(hello + G4_HDR_SIZE + 14, wmm);
    }
    if (write_full(fd, hello, sizeof hello)) { g4dec_free(&s.dec); free(s.fb); return; }

    {
        char msg[256];
        snprintf(msg, sizeof msg, "Showing your Mac: %dx%d, %d-bit colour, %d cache tiles. Command-Escape returns here.",
                 s.W, s.H, s.depth, s.dec.cache_slots);
        report(G4C_CONNECTED, msg);
    }
    if (opt_test) fprintf(stderr, "TEST MODE: decoding without drawing; snapshot every 5 s to %s\n", snapshot_path());
    {
        int need_draw = 0, new_frames = 0;
        uint32_t ack = 0;
        double last_snap = now_ms();
        s.stat_t0 = now_ms();

        for (;;) {
            fd_set rf;
            struct timeval tv;
            double burst_start = now_ms();
            int alive = 1;

            /* drain everything that is already waiting (coalesces cursor
               moves and back-to-back frames), but never for longer than
               ~10 ms before putting something on screen */
            for (;;) {
                int n;
                FD_ZERO(&rf); FD_SET(fd, &rf);
                tv.tv_sec = 0;
                tv.tv_usec = need_draw ? 0 : 50000;
                n = select(fd + 1, &rf, NULL, NULL, &tv);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) break;
                if (handle_message(&s, &need_draw, &ack)) { alive = 0; break; }
                if (now_ms() - burst_start > 10.0) break;
            }
            if (!alive) break;

            /* take over the screen only once there is a picture to show, so a
               connection that fails early never blanks the G4 */
            if (need_draw && !opt_test && !s.render && s.got_frame) {
                s.gl_tried = 1;
                /* best first: GPU, then writing the framebuffer directly
                   (faster than Apple's software OpenGL), then software GL */
                if (gl_setup(&s, 0) == 0) {
                    s.render = 1;
                    upload_cursor(&s);
                } else {
                    gl_teardown(&s);
                    if (raw_setup(&s) == 0) s.render = 2;
                    else {
                        gl_teardown(&s);
                        if (gl_setup(&s, 1) == 0) { s.render = 1; upload_cursor(&s); }
                        else { report(G4C_ERROR, "This Mac can't show the picture: no OpenGL and no usable framebuffer."); break; }
                    }
                }
            }

            if (need_draw) {
                if (opt_test) {
                    g4dec_reset_dirty(&s.dec);
                    s.stat_draws++;
                    if (ack) new_frames = 1;
                } else if (s.render == 1) {
                    draw(&s);
                } else if (s.render == 2) {
                    raw_draw(&s);
                }
                need_draw = 0;
                if (ack) {                                /* frame is on screen */
                    if (send_u32_msg(fd, G4_MSG_ACK, ack)) break;
                    ack = 0;
                }
            }

            if (opt_test && new_frames && now_ms() - last_snap > 5000) {
                last_snap = now_ms();
                new_frames = 0;
                save_snapshot(&s);
            }

            if (now_ms() - last_keys > 100) {
                last_keys = now_ms();
                if (quit_keys_down()) {
                    if (app_mode) g4c_set_paused(1);   /* back to the app window */
                    else quit_requested = 1;
                    break;
                }
                if (quit_requested) break;
            }

            /* the G4 sees no keyboard/mouse while the M1 is in use: tell the
               power manager the user is active so the display (and screen
               saver) never kicks in mid-session */
            if (now_ms() - last_awake > 20000) {
                last_awake = now_ms();
                UpdateSystemActivity(UsrActivity);
            }

            if ((opt_verbose || opt_test) && now_ms() - s.stat_t0 > 5000) {
                double secs = (now_ms() - s.stat_t0) / 1000.0;
                double nd = s.stat_draws ? (double)s.stat_draws : 1.0;
                fprintf(stderr, "%.1f frames/s, %.1f draws/s, %.0f KB/s in, decode %.1f ms/frame, "
                                "GPU wait %.1f ms/frame, upload %.1f ms + %.0f KB/draw, flush %.1f ms/draw\n",
                        s.stat_frames / secs, s.stat_draws / secs, s.stat_bytes / 1024.0 / secs,
                        s.stat_frames ? s.stat_decode_ms / s.stat_frames : 0.0,
                        s.stat_frames ? s.stat_wait_ms / s.stat_frames : 0.0,
                        s.stat_upload_ms / nd, s.stat_upload_kb / nd, s.stat_flush_ms / nd);
                s.stat_t0 = now_ms(); s.stat_frames = s.stat_draws = 0;
                s.stat_bytes = s.stat_decode_ms = 0;
                s.stat_wait_ms = s.stat_upload_ms = s.stat_flush_ms = s.stat_upload_kb = 0;
            }
        }
    }
    if (s.gl_tried) gl_teardown(&s);
    free(s.cur_px);
    g4dec_free(&s.dec);
    free(s.fb);
    free(s.msg);
    fprintf(stderr, "host disconnected\n");
}

/* ------------------------------------------------------------------ */
/* server loop: listen, advertise on Bonjour, run one session at a time */

static g4c_status_fn status_hook = NULL;
static volatile int g_paused = 0;
static int lfd = -1;
static DNSServiceRef bonjour = NULL;
static char bonjour_name[256];

void g4c_set_status_hook(g4c_status_fn fn) { status_hook = fn; }
void g4c_set_depth(int depth) { opt_depth = (depth == 16 || depth == 32) ? depth : 0; }
void g4c_set_paused(int paused) { g_paused = paused; }
int  g4c_is_paused(void) { return g_paused; }
void g4c_set_app_mode(int on) { app_mode = on; }
void g4c_stop(void) { quit_requested = 1; }

/* report a state change (deduplicated) to the app, or to stderr */
static void report(int state, const char *text)
{
    static int last_state = -1;
    static char last_text[512];
    if (state == last_state && !strcmp(text, last_text)) return;
    last_state = state;
    snprintf(last_text, sizeof last_text, "%s", text);
    fprintf(stderr, "%s\n", text);
    if (status_hook) status_hook(state, text);
}

static void report_waiting(void)
{
    char msg[512];
    if (bonjour_name[0])
        snprintf(msg, sizeof msg, "Waiting for your Mac. Open PowerPC Display on it; this Mac shows up as \"%s\".", bonjour_name);
    else
        snprintf(msg, sizeof msg, "Waiting for your Mac on port %d.", opt_port);
    report(G4C_WAITING, msg);
}

static void DNSSD_API bonjour_cb(DNSServiceRef ref, DNSServiceFlags flags, DNSServiceErrorType err,
                                 const char *name, const char *type, const char *domain, void *ctx)
{
    (void)ref; (void)flags; (void)type; (void)domain; (void)ctx;
    if (err == kDNSServiceErr_NoError) snprintf(bonjour_name, sizeof bonjour_name, "%s", name);
    else fprintf(stderr, "Bonjour error %d\n", (int)err);
}

static int open_listener(void)
{
    int one = 1;
    struct sockaddr_in sa;
    lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) return -1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)opt_port);
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(lfd, (struct sockaddr *)&sa, sizeof sa) || listen(lfd, 1)) {
        close(lfd); lfd = -1;
        return -1;
    }
    /* Bonjour: name NULL = this Mac's computer name (System Preferences › Sharing) */
    bonjour_name[0] = 0;
    if (DNSServiceRegister(&bonjour, 0, 0, NULL, "_g4display._tcp", NULL, NULL,
                           htons((unsigned short)opt_port), 0, NULL, bonjour_cb, NULL) != kDNSServiceErr_NoError) {
        fprintf(stderr, "Bonjour registration failed; connect from the M1 by IP address instead\n");
        bonjour = NULL;
    }
    return 0;
}

static void close_listener(void)
{
    if (bonjour) { DNSServiceRefDeallocate(bonjour); bonjour = NULL; }
    if (lfd >= 0) { close(lfd); lfd = -1; }
    bonjour_name[0] = 0;
}

int g4c_server_run(void)
{
    signal(SIGPIPE, SIG_IGN);

    while (!quit_requested) {
        struct sockaddr_in peer;
        socklen_t pl = sizeof peer;
        fd_set rf;
        struct timeval tv;
        int fd, n, bfd, maxfd;

        if (g_paused) {
            close_listener();
            report(G4C_PAUSED, "Paused. Your Mac can't connect until you click Resume.");
            while (g_paused && !quit_requested) usleep(200000);
            continue;
        }
        if (lfd < 0 && open_listener()) {
            report(G4C_ERROR, "Can't listen for your Mac (is another copy of PowerPC Display running?)");
            sleep(3);
            continue;
        }
        report_waiting();

        /* wait for a connection while servicing Bonjour; wake every 0.5 s
           to notice Pause/Quit */
        bfd = bonjour ? DNSServiceRefSockFD(bonjour) : -1;
        maxfd = lfd > bfd ? lfd : bfd;
        FD_ZERO(&rf); FD_SET(lfd, &rf);
        if (bfd >= 0) FD_SET(bfd, &rf);
        tv.tv_sec = 0; tv.tv_usec = 500000;
        n = select(maxfd + 1, &rf, NULL, NULL, &tv);
        if (n <= 0) continue;
        if (bfd >= 0 && FD_ISSET(bfd, &rf) && DNSServiceProcessResult(bonjour) != kDNSServiceErr_NoError) {
            DNSServiceRefDeallocate(bonjour);
            bonjour = NULL;
        }
        if (!FD_ISSET(lfd, &rf)) continue;

        fd = accept(lfd, (struct sockaddr *)&peer, &pl);
        if (fd < 0) continue;
        fprintf(stderr, "connection from %s\n", inet_ntoa(peer.sin_addr));
        run_session(fd);
        close(fd);
    }
    close_listener();
    return 0;
}

#ifndef G4_APP
int main(int argc, char **argv)
{
    int i;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-16")) opt_depth = 16;
        else if (!strcmp(argv[i], "-32")) opt_depth = 32;
        else if (!strcmp(argv[i], "-v")) opt_verbose = 1;
        else if (!strcmp(argv[i], "-t")) opt_test = 1;
        else if (!strcmp(argv[i], "-p") && i + 1 < argc) opt_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-c") && i + 1 < argc) opt_slots = atoi(argv[++i]);
        else {
            fprintf(stderr, "usage: %s [-16|-32] [-v] [-t] [-p port] [-c cache_tiles (0=off)]\n"
                            "  default: colour depth and cache size picked for this Mac\n", argv[0]);
            return 2;
        }
    }
    fprintf(stderr, "PowerPC Display on %s\n", g4c_machine_summary());
    fprintf(stderr, "automatic choices: %d-bit colour, %d cache tiles\n", g4c_auto_depth(), auto_slots());
    fprintf(stderr, "Cmd+Esc quits while showing the other Mac\n");
    return g4c_server_run();
}
#endif
