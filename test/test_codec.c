/*
 * test_codec.c — round-trip and robustness tests for g4codec.
 *
 * Renders a synthetic desktop (editor window full of "text", menubar,
 * gradient wallpaper), drives it through typing, scrolling (with and without
 * dirty-rect hints), smooth pixel scrolling, a photo-like region and full
 * screen changes. After every frame the decoder's framebuffer must equal the
 * encoder's current frame byte for byte. Then the decoder is fuzzed with
 * corrupted messages.
 */
#include "g4codec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define W 1440
#define H 900
#define WX 160
#define WY 80
#define WW 1140
#define WH 740
#define LINE_H 18
#define NLINES 4000

static uint8_t bgra[W * H * 4];
static uint32_t lineseed[NLINES];
static int theme;
static uint32_t clk = 1000, clk_step = 150;   /* simulated wall clock */

static uint32_t rng_state = 12345;
static uint32_t rnd(void) { rng_state ^= rng_state << 13; rng_state ^= rng_state >> 17; rng_state ^= rng_state << 5; return rng_state; }

static void px(int x, int y, int r, int g, int b)
{
    uint8_t *p = bgra + ((size_t)y * W + x) * 4;
    p[0] = (uint8_t)b; p[1] = (uint8_t)g; p[2] = (uint8_t)r; p[3] = 255;
}

/* draw one text line: glyph cells 8x14 with pseudo-antialiased pixels */
static void draw_line(int lineno, int y0, int clipy0, int clipy1)
{
    uint32_t s = lineseed[lineno % NLINES] * 2654435761u + 1;
    int len = (int)(s % 110), c, gx, gy;
    int indent = (int)((s >> 8) % 6) * 2;
    for (c = 0; c < len; c++) {
        uint32_t gs = s ^ (uint32_t)(c * 0x9E3779B9u);
        if (c < indent) continue;
        if ((gs >> 3) % 7 == 0) continue;                 /* spaces */
        for (gy = 2; gy < 16; gy++) {
            int y = y0 + gy;
            if (y < clipy0 || y >= clipy1) continue;
            for (gx = 1; gx < 8; gx++) {
                uint32_t bit = (gs * (uint32_t)(gy * 31 + gx * 7)) >> 27;
                int x = WX + 48 + c * 8 + gx;
                if (x >= WX + WW) break;
                if (bit < 9) {
                    int v = 120 + (int)(bit * 15);
                    if (theme) px(x, y, 255 - v, 255 - v, 255 - v);
                    else       px(x, y, v, v + 10, (int)(v * 0.9));
                }
            }
        }
    }
}

/* render desktop with the editor scrolled to pixel offset `scroll` */
static void render(int scroll)
{
    int x, y, first, l;
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++) {
            if (y < 22) px(x, y, 236, 236, 236);
            else if (theme) px(x, y, 40 + x / 24, 60, 90 + y / 12);
            else px(x, y, 20 + y / 10, 30 + x / 30, 70);
        }
    /* window chrome + background */
    for (y = WY - 24; y < WY; y++) for (x = WX; x < WX + WW; x++) px(x, y, 210, 210, 210);
    for (y = WY; y < WY + WH; y++) for (x = WX; x < WX + WW; x++) {
        if (x < WX + 44) px(x, y, theme ? 230 : 37, theme ? 230 : 37, theme ? 230 : 40);
        else px(x, y, theme ? 250 : 30, theme ? 250 : 30, theme ? 250 : 32);
    }
    first = scroll / LINE_H;
    for (l = first; l <= first + WH / LINE_H + 1; l++)
        draw_line(l, WY + l * LINE_H - scroll, WY, WY + WH);
}

/* a browser-like page: render() plus a sticky header that doesn't scroll
   and an overlay scrollbar whose thumb moves with the page (macOS shows it
   while scrolling) — both change every row's right edge / top band */
#define HDR_H 44
#define SB_W 12
static void render_browser(int scroll)
{
    int x, y, ty;
    render(scroll);
    for (y = WY; y < WY + HDR_H; y++) for (x = WX; x < WX + WW; x++)
        px(x, y, 246, 246, 246);
    draw_line(7, WY + 14, WY, WY + HDR_H);                  /* header title text */
    ty = WY + HDR_H + (scroll / 6) % (WH - HDR_H - 120);
    for (y = WY + HDR_H; y < WY + WH; y++) for (x = WX + WW - SB_W - 2; x < WX + WW - 2; x++)
        if (y >= ty && y < ty + 120) px(x, y, 120, 120, 120);
        else px(x, y, 60, 60, 62);
}

static void photo(int x0, int y0, int w, int h)
{
    int x, y;
    for (y = y0; y < y0 + h; y++) for (x = x0; x < x0 + w; x++)
        px(x, y, (int)(rnd() & 255), (int)((x * 3 + rnd() % 40) & 255), (int)((y * 2 + rnd() % 40) & 255));
}

typedef struct { const char *name; int frames; double bytes, raw; int copies, hits, stores; double dec_ms; } scen;

static void menu(int x0, int y0, int w, int h)
{
    int x, y, l;
    for (y = y0; y < y0 + h; y++) for (x = x0; x < x0 + w; x++) px(x, y, 245, 245, 245);
    for (l = 0; l < h / 22; l++) for (y = y0 + l * 22 + 6; y < y0 + l * 22 + 16; y++)
        for (x = x0 + 12; x < x0 + 12 + (int)((l * 37) % 120) + 60; x++) if ((x * 7 + y) % 5) px(x, y, 30, 30, 30);
}

static void caret(int on)
{
    int y;
    if (on) for (y = WY + 12 * LINE_H; y < WY + 13 * LINE_H; y++) { px(WX + 300, y, 255, 255, 255); px(WX + 301, y, 255, 255, 255); }
}


static int run(int depth, int slots)
{
    g4_encoder *e = g4enc_create(W, H, depth);
    int bpp = depth == 16 ? 2 : 4;
    uint8_t *fb = (uint8_t *)calloc((size_t)W * H, bpp);
    g4_decoder d;
    uint32_t fid = 0;
    int scroll = 0, i, fails = 0, si = 0;
    scen sc[20];
    memset(sc, 0, sizeof sc);
    g4dec_init(&d, fb, W, H, depth);
    g4dec_enable_cache(&d, slots);
    g4enc_enable_cache(e, slots);
    for (i = 0; i < NLINES; i++) lineseed[i] = (uint32_t)i * 7919u + 17;
    theme = 0;

#define STEP(hint) do {                                                          \
        const uint8_t *msg; size_t n; g4_enc_stats st; uint32_t got;             \
        clock_t t0; int rc;                                                       \
        g4enc_ingest_bgra(e, bgra, W * 4);                                        \
        clk += clk_step; g4enc_set_time(e, clk);                                  \
        if (hint == 1) g4enc_add_hint(e, WX, WY, WW, WH);                         \
        if (hint == 2) { g4enc_add_hint(e, WX, WY + HDR_H, WW - SB_W - 2, WH - HDR_H); \
                         g4enc_add_hint(e, WX + WW - SB_W - 2, WY, SB_W + 2, WH); }    \
        n = g4enc_encode(e, ++fid, &msg, &st);                                    \
        if (n) {                                                                  \
            t0 = clock();                                                         \
            rc = g4dec_frame(&d, msg + G4_HDR_SIZE, n - G4_HDR_SIZE, &got);       \
            sc[si].dec_ms += (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC;    \
            if (rc || got != fid) { printf("  decode error %d\n", rc); fails++; } \
        }                                                                         \
        sc[si].frames++; sc[si].bytes += (double)n; sc[si].raw += st.raw_bytes;   \
        sc[si].copies += (int)st.copies; sc[si].hits += (int)st.cache_hits;      \
        sc[si].stores += (int)st.cache_stores;                                    \
        if (memcmp(fb, g4enc_current(e), (size_t)W * H * bpp)) {                  \
            printf("  MISMATCH in %s frame %d\n", sc[si].name, sc[si].frames);    \
            fails++;                                                              \
        }                                                                         \
        if (memcmp(g4enc_model(e), g4enc_current(e), (size_t)W * H * bpp)) {      \
            printf("  MODEL DRIFT in %s\n", sc[si].name); fails++;                \
        }                                                                         \
    } while (0)

    sc[si].name = "first full frame";        render(0); STEP(0); si++;

    sc[si].name = "typing (1 char/frame)";
    for (i = 0; i < 30; i++) { lineseed[12] += 1; render(0); STEP(1); }
    si++;

    sc[si].name = "scroll 3 lines + hint";
    for (i = 0; i < 30; i++) { scroll += 3 * LINE_H; render(scroll); STEP(1); }
    si++;

    sc[si].name = "scroll 1 line, no hint";
    for (i = 0; i < 30; i++) { scroll += LINE_H; render(scroll); STEP(0); }
    si++;

    sc[si].name = "smooth scroll 7px+hint";
    for (i = 0; i < 30; i++) { scroll += 7; render(scroll); STEP(1); }
    si++;

    sc[si].name = "scroll up 2 lines+hint";
    for (i = 0; i < 20; i++) { scroll -= 2 * LINE_H; render(scroll); STEP(1); }
    si++;

    sc[si].name = "browser 40px + hint";
    for (i = 0; i < 30; i++) { scroll += 40; render_browser(scroll); STEP(1); }
    si++;

    sc[si].name = "browser 40px split hints";
    for (i = 0; i < 30; i++) { scroll += 40; render_browser(scroll); STEP(2); }
    si++;

    sc[si].name = "browser 40px no hint";
    for (i = 0; i < 30; i++) { scroll += 40; render_browser(scroll); STEP(0); }
    si++;

    sc[si].name = "browser inertia + hint";
    {
        static const int dys[] = { 90, 70, 52, 38, 27, 19, 13, 9, 6, 4, 3, 2, 1, 1 };
        for (i = 0; i < (int)(sizeof dys / sizeof dys[0]); i++) { scroll += dys[i]; render_browser(scroll); STEP(1); }
    }
    si++;
    render(scroll); STEP(0);

    sc[si].name = "video 400x300 @30fps";
    clk_step = 33;
    for (i = 0; i < 10; i++) { render(scroll); photo(500, 300, 400, 300); STEP(0); }
    clk_step = 150;
    si++;

    sc[si].name = "full-screen change";
    for (i = 0; i < 6; i++) { theme ^= 1; render(scroll + i * 400); STEP(0); }
    si++;

    sc[si].name = "menu open/close x10";
    for (i = 0; i < 10; i++) {
        render(scroll); menu(300, 22, 260, 400); STEP(0);
        render(scroll); STEP(0);
    }
    si++;

    sc[si].name = "app switch A<->B x10";
    for (i = 0; i < 20; i++) { theme = i & 1; render(i & 1 ? 9000 : scroll); STEP(0); }
    theme = 0;
    si++;

    sc[si].name = "caret blink x20";
    for (i = 0; i < 20; i++) { render(scroll); caret(i & 1); STEP(0); }
    si++;

    printf("depth %d, cache %d slots:\n", depth, slots);
    for (i = 0; i < si; i++) {
        printf("  %-24s %3d fr  avg %7.1f KB wire  (%6.1f KB changed px)  copies %3d  hits %5d  stores %5d  dec %.2f ms\n",
               sc[i].name, sc[i].frames, sc[i].bytes / sc[i].frames / 1024.0,
               sc[i].raw / sc[i].frames / 1024.0,
               sc[i].copies, sc[i].hits, sc[i].stores, sc[i].dec_ms / sc[i].frames);
    }

    /* ---- fuzz: corrupt real frames, decoder must reject or survive ---- */
    {
        int k, rejected = 0;
        size_t cap = 0; uint8_t *buf = NULL;
        render(0); g4enc_force_full(e);
        for (k = 0; k < 3000; k++) {
            const uint8_t *msg; size_t n; g4_enc_stats st;
            if (k % 100 == 0) { theme ^= 1; render(k * 3); g4enc_ingest_bgra(e, bgra, W * 4); }
            else { lineseed[k % 50] += 3; render(k % 7 * LINE_H); g4enc_ingest_bgra(e, bgra, W * 4); }
            n = g4enc_encode(e, ++fid, &msg, &st);
            if (!n) continue;
            if (n > cap) { cap = n; buf = (uint8_t *)realloc(buf, cap); }
            memcpy(buf, msg, n);
            /* real decode keeps fb in sync; corrupted decode goes to a scratch decoder */
            g4dec_frame(&d, msg + G4_HDR_SIZE, n - G4_HDR_SIZE, NULL);
            {
                uint8_t *fb2 = (uint8_t *)malloc((size_t)W * H * bpp);
                g4_decoder d2; int flips = 1 + (int)(rnd() % 8), f;
                size_t len = n - G4_HDR_SIZE;
                g4dec_init(&d2, fb2, W, H, depth);
                g4dec_enable_cache(&d2, slots);
                for (f = 0; f < flips; f++) buf[G4_HDR_SIZE + rnd() % len] ^= (uint8_t)(1u << (rnd() % 8));
                if (rnd() % 4 == 0) len = rnd() % len;
                if (g4dec_frame(&d2, buf + G4_HDR_SIZE, len, NULL)) rejected++;
                g4dec_free(&d2); free(fb2);
            }
        }
        printf("  fuzz: 3000 corrupted frames survived (%d rejected as malformed)\n", rejected);
        free(buf);
    }

    g4dec_free(&d); free(fb); g4enc_destroy(e);
    return fails;
}

static int lz4_selftest(void)
{
    static uint8_t a[300000], c[310000], b[300000];
    int t, fails = 0;
    for (t = 0; t < 200; t++) {
        size_t n = rnd() % sizeof a, i, cl;
        int mode = t % 4;
        for (i = 0; i < n; i++)
            a[i] = mode == 0 ? (uint8_t)rnd() : mode == 1 ? (uint8_t)(i % 4 == 0 ? 255 : 30)
                 : mode == 2 ? (uint8_t)((i / 97) & 3) : (uint8_t)(rnd() % 3);
        cl = g4_lz4_compress(a, n, c, sizeof c);
        if (!cl || g4_lz4_decompress(c, cl, b, n) || memcmp(a, b, n)) { printf("lz4 fail n=%zu mode=%d\n", n, mode); fails++; }
    }
    printf("lz4: 200 round trips %s\n", fails ? "FAILED" : "ok");
    return fails;
}

/* 4:2:2 video path: the client must always equal the encoder's model, and
   once the video stops everything must become exact again */
static int one_yuv(int w, int h, int with_cache)
{
    g4_encoder *e = g4enc_create(w, h, 32);
    uint8_t *fb = (uint8_t *)calloc((size_t)w * h, 4);
    uint8_t *src = (uint8_t *)malloc((size_t)w * h * 4);
    g4_decoder d;
    uint32_t fid = 0, t = 5000;
    int i, fails = 0, x, y, yuvs = 0, refreshes = 0, fuzzed = 0;
    double vbytes = 0;
    g4dec_init(&d, fb, w, h, 32);
    if (with_cache) { g4dec_enable_cache(&d, 512); g4enc_enable_cache(e, 512); }
    g4enc_set_yuv(e, 1);

#define YSTEP(expect_exact) do {                                                     \
        const uint8_t *msg; size_t n; g4_enc_stats st;                                \
        g4enc_ingest_bgra(e, src, (size_t)w * 4); g4enc_set_time(e, t);               \
        n = g4enc_encode(e, ++fid, &msg, &st);                                        \
        yuvs += st.yuvs; refreshes += st.refreshes; if (st.yuvs) vbytes += n;         \
        if (n) {                                                                       \
            int rc = g4dec_frame(&d, msg + G4_HDR_SIZE, n - G4_HDR_SIZE, NULL);       \
            if (rc) { printf("yuv %dx%d: decode error %d\n", w, h, rc); fails++; }    \
            if (fuzzed < 300) {                                                        \
                uint8_t *buf = (uint8_t *)malloc(n), *fb2 = (uint8_t *)calloc((size_t)w * h, 4); \
                g4_decoder d2; int k;                                                  \
                memcpy(buf, msg, n);                                                   \
                for (k = 0; k < 6; k++) buf[G4_HDR_SIZE + rnd() % (n - G4_HDR_SIZE)] ^= (uint8_t)(1 + rnd() % 255); \
                g4dec_init(&d2, fb2, w, h, 32); if (with_cache) g4dec_enable_cache(&d2, 512); \
                g4dec_frame(&d2, buf + G4_HDR_SIZE, n - G4_HDR_SIZE, NULL);           \
                g4dec_free(&d2); free(fb2); free(buf); fuzzed++;                       \
            }                                                                          \
        }                                                                              \
        if (memcmp(fb, g4enc_model(e), (size_t)w * h * 4)) {                           \
            printf("yuv %dx%d frame %u: client != model\n", w, h, fid); fails++; }    \
        if ((expect_exact) && memcmp(fb, g4enc_current(e), (size_t)w * h * 4)) {      \
            printf("yuv %dx%d frame %u: not exact after refresh\n", w, h, fid); fails++; } \
    } while (0)

    /* desktop-ish background */
    for (y = 0; y < h; y++) for (x = 0; x < w; x++) {
        uint8_t *q = src + ((size_t)y * w + x) * 4;
        q[0] = (uint8_t)(x * 3); q[1] = (uint8_t)(y * 2); q[2] = (uint8_t)((x ^ y) & 255); q[3] = 255;
    }
    YSTEP(1);
    /* video in the middle at 30 fps, text changing elsewhere now and then */
    for (i = 0; i < 40; i++) {
        int vx = w / 5 + 1, vy = h / 5, vw = w * 3 / 5, vh = h * 3 / 5;
        for (y = vy; y < vy + vh; y++) for (x = vx; x < vx + vw; x++) {
            uint8_t *q = src + ((size_t)y * w + x) * 4;
            q[0] = (uint8_t)(rnd()); q[1] = (uint8_t)(x + i * 5); q[2] = (uint8_t)(y * 3 + i); q[3] = 255;
        }
        if (i % 7 == 0) { uint8_t *q = src + 4 * (size_t)(i % w); q[0] ^= 0x55; }   /* "caret" */
        t += 33;
        YSTEP(0);
    }
    /* video stops: within the refresh window nothing is resent, then it's exact */
    {
        int due = g4enc_refresh_due(e, t);
        if (due < 0) { printf("yuv %dx%d: no refresh pending after video\n", w, h); fails++; }
    }
    t += 100; YSTEP(0);
    t += 400; YSTEP(1);
    if (g4enc_refresh_due(e, t) >= 0) { printf("yuv %dx%d: refresh still pending\n", w, h); fails++; }
    if (!yuvs || !refreshes) { printf("yuv %dx%d: video path not exercised (yuv %d, refresh %d)\n", w, h, yuvs, refreshes); fails++; }

    /* off again: everything lossless */
    g4enc_set_yuv(e, 0);
    for (i = 0; i < 5; i++) { src[(size_t)(h / 2) * w * 4 + 4 * (size_t)i] ^= 0x33; t += 33; YSTEP(1); }

    printf("yuv %dx%d%s: %d video rects, %d tiles refreshed, avg video frame %.0f KB, %s\n", w, h,
           with_cache ? " +cache" : "", yuvs, refreshes, yuvs ? vbytes / 1024 / 40 : 0.0, fails ? "FAILED" : "ok");
#undef YSTEP
    g4dec_free(&d); free(fb); free(src); g4enc_destroy(e);
    return fails;
}

/* dirty-rect ingestion: only the rects that changed are converted and
   compared; the client must still match the true screen exactly */
static int run_rects(int depth, int yuv)
{
    g4_encoder *e = g4enc_create(W, H, depth), *ref = g4enc_create(W, H, depth);
    int bpp = depth == 16 ? 2 : 4, i, k, fails = 0;
    uint8_t *fb = (uint8_t *)calloc((size_t)W * H, bpp);
    g4_decoder d;
    uint32_t fid = 0, t = 1000;
    double bytes = 0;
    g4dec_init(&d, fb, W, H, depth);
    g4dec_enable_cache(&d, 1024); g4enc_enable_cache(e, 1024);
    g4enc_set_yuv(e, yuv);
    for (i = 0; i < NLINES; i++) lineseed[i] = (uint32_t)i * 7919u + 17;
    theme = 0; render(0);
    g4enc_ingest_bgra(e, bgra, W * 4);                 /* first frame: everything */
    for (i = 0; i < 160; i++) {
        const uint8_t *msg; size_t n; g4_enc_stats st;
        int nr = 1 + (int)(rnd() % 4);
        if (i == 60) { render(3 * LINE_H); g4enc_ingest_bgra_rect(e, bgra, W * 4, 0, 0, W, H);   /* scroll (render repaints all) */
                       g4enc_add_hint(e, WX, WY, WW, WH); }
        for (k = 0; k < nr; k++) {
            int rx = (int)(rnd() % (W - 40)), ry = (int)(rnd() % (H - 40));
            int rw = 1 + (int)(rnd() % 300), rh = 1 + (int)(rnd() % 200);
            if (i >= 100 && i < 130 && k == 0) { rx = 300; ry = 200; rw = 320; rh = 240; photo(rx, ry, rw, rh); }  /* video */
            else {
                int x, y, c = (int)(rnd() & 255);
                if (rx + rw > W) rw = W - rx;
                if (ry + rh > H) rh = H - ry;
                for (y = ry; y < ry + rh; y++) for (x = rx; x < rx + rw; x++) if ((x ^ y) & 1) px(x, y, c, 255 - c, (c * 3) & 255);
            }
            g4enc_ingest_bgra_rect(e, bgra, W * 4, rx, ry, rw, rh);
        }
        if (i % 50 == 49) g4enc_ingest_bgra(e, bgra, W * 4);   /* periodic full check */
        t += 33; g4enc_set_time(e, t);
        n = g4enc_encode(e, ++fid, &msg, &st);
        bytes += n;
        if (n && g4dec_frame(&d, msg + G4_HDR_SIZE, n - G4_HDR_SIZE, NULL)) { printf("rects: decode error\n"); fails++; }
        g4enc_ingest_bgra(ref, bgra, W * 4);
        if (memcmp(g4enc_current(e), g4enc_current(ref), (size_t)W * H * bpp)) { printf("rects frame %d: current != screen\n", i); fails++; break; }
        if (memcmp(fb, g4enc_model(e), (size_t)W * H * bpp)) { printf("rects frame %d: client != model\n", i); fails++; break; }
    }
    /* video stopped: refresh makes it exact */
    { const uint8_t *msg; size_t n; g4_enc_stats st;
      t += 500; g4enc_set_time(e, t); n = g4enc_encode(e, ++fid, &msg, &st);
      if (n) g4dec_frame(&d, msg + G4_HDR_SIZE, n - G4_HDR_SIZE, NULL);
      if (memcmp(fb, g4enc_current(ref), (size_t)W * H * bpp)) { printf("rects: not exact at the end\n"); fails++; } }
    printf("dirty rects, depth %d%s: 160 frames, %.0f KB total, %s\n", depth, yuv ? " +video" : "", bytes / 1024, fails ? "FAILED" : "ok");
    g4dec_free(&d); free(fb); g4enc_destroy(e); g4enc_destroy(ref);
    return fails;
}

static int run_yuv(void)
{
    int fails = 0, k;
    /* the colour maths must round-trip greys exactly and stay close otherwise */
    for (k = 0; k < 256; k++) {
        uint8_t a[4] = { 255, (uint8_t)k, (uint8_t)k, (uint8_t)k }, o[4], r0[4], r1[4];
        g4_yuv_from_pair(a, a, o); g4_yuv_to_pair(o, r0, r1);
        if (abs(r0[1] - k) > 1 || abs(r0[2] - k) > 1 || abs(r0[3] - k) > 1) { printf("yuv grey %d -> %d,%d,%d\n", k, r0[1], r0[2], r0[3]); fails++; break; }
    }
    fails += one_yuv(W, H, 1);
    fails += one_yuv(W, H, 0);
    fails += one_yuv(101, 67, 1);     /* odd width: last pixel paired with itself */
    return fails;
}

int main(int argc, char **argv)
{
    int fails = lz4_selftest();
    if (argc > 1 && !strcmp(argv[1], "yuv")) {          /* just the video path + dirty rects */
        fails += run_yuv();
        fails += run_rects(32, 0); fails += run_rects(32, 1); fails += run_rects(16, 0);
        printf(fails ? "\nFAILED (%d)\n" : "\nALL TESTS PASSED\n", fails);
        return fails != 0;
    }
    fails += run(32, 0);
    fails += run(32, G4_DEFAULT_SLOTS);
    fails += run(16, G4_DEFAULT_SLOTS);
    fails += run_yuv();
    fails += run_rects(32, 0); fails += run_rects(32, 1); fails += run_rects(16, 0);
    printf(fails ? "\nFAILED (%d)\n" : "\nALL TESTS PASSED\n", fails);
    return fails != 0;
}
