/*
 * g4codec.c — LZ4 block codec, change-only screen encoder (host) and
 * decoder (G4). See g4codec.h for the wire format.
 */
#include "g4codec.h"

#include <stdlib.h>
#include <string.h>

/* ======================================================================
 * LZ4 block format
 * ==================================================================== */

#define LZ4_HASH_LOG   14
#define LZ4_MINMATCH   4
#define LZ4_MFLIMIT    12   /* last match must start >= 12 bytes before end */
#define LZ4_LASTLIT    5    /* last 5 bytes are always literals              */

static inline uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline uint32_t lz4_hash(uint32_t v) { return (v * 2654435761u) >> (32 - LZ4_HASH_LOG); }

size_t g4_lz4_bound(size_t n) { return n + n / 255 + 16; }

static uint8_t *lz4_put_len(uint8_t *op, size_t len) {
    while (len >= 255) { *op++ = 255; len -= 255; }
    *op++ = (uint8_t)len;
    return op;
}

size_t g4_lz4_compress(const uint8_t *src, size_t n, uint8_t *dst, size_t cap)
{
    uint32_t htab[1u << LZ4_HASH_LOG];
    const uint8_t *ip = src, *anchor = src;
    const uint8_t *const end = src + n;
    uint8_t *op = dst;

    if (cap < g4_lz4_bound(n)) return 0;

    if (n >= LZ4_MFLIMIT + 1) {
        const uint8_t *const mflimit = end - LZ4_MFLIMIT;
        const uint8_t *const matchlimit = end - LZ4_LASTLIT;
        unsigned misses = 0;
        memset(htab, 0, sizeof htab);

        while (ip < mflimit) {
            uint32_t seq = rd32(ip);
            uint32_t h = lz4_hash(seq);
            const uint8_t *ref = src + htab[h];
            htab[h] = (uint32_t)(ip - src);

            if (ref < ip && (size_t)(ip - ref) <= 65535 && rd32(ref) == seq) {
                const uint8_t *m = ip + LZ4_MINMATCH, *r = ref + LZ4_MINMATCH;
                size_t lit = (size_t)(ip - anchor), ml, off = (size_t)(ip - ref);
                uint8_t *token;

                /* extend backwards over pending literals */
                while (ip > anchor && ref > src && ip[-1] == ref[-1]) { ip--; ref--; lit--; }
                m = ip + LZ4_MINMATCH; r = ref + LZ4_MINMATCH;
                while (m < matchlimit && *m == *r) { m++; r++; }
                ml = (size_t)(m - ip) - LZ4_MINMATCH;

                token = op++;
                if (lit >= 15) { *token = 15 << 4; op = lz4_put_len(op, lit - 15); }
                else             *token = (uint8_t)(lit << 4);
                memcpy(op, anchor, lit); op += lit;
                *op++ = (uint8_t)off; *op++ = (uint8_t)(off >> 8);
                if (ml >= 15) { *token |= 15; op = lz4_put_len(op, ml - 15); }
                else            *token |= (uint8_t)ml;

                ip = m; anchor = ip; misses = 0;
                if (ip < mflimit) htab[lz4_hash(rd32(ip - 2))] = (uint32_t)(ip - 2 - src);
            } else {
                /* skip faster through incompressible data */
                ip += 1 + (misses++ >> 6);
            }
        }
    }

    {   /* final literals */
        size_t lit = (size_t)(end - anchor);
        if (lit >= 15) { *op++ = 15 << 4; op = lz4_put_len(op, lit - 15); }
        else             *op++ = (uint8_t)(lit << 4);
        memcpy(op, anchor, lit); op += lit;
    }
    return (size_t)(op - dst);
}

int g4_lz4_decompress(const uint8_t *src, size_t slen, uint8_t *dst, size_t dlen)
{
    const uint8_t *ip = src, *const iend = src + slen;
    uint8_t *op = dst, *const oend = dst + dlen;

    while (ip < iend) {
        unsigned token = *ip++;
        size_t lit = token >> 4, ml, off;
        const uint8_t *ref;

        if (lit == 15) {
            unsigned b;
            do { if (ip >= iend) return -1; b = *ip++; lit += b; } while (b == 255);
        }
        if ((size_t)(iend - ip) < lit || (size_t)(oend - op) < lit) return -1;
        if (lit <= 16 && iend - ip >= 16 && oend - op >= 16) {
            /* short literal run (the common case): fixed-size copies inline to
               plain loads/stores instead of a memcpy call */
            memcpy(op, ip, 8); memcpy(op + 8, ip + 8, 8);
        } else {
            memcpy(op, ip, lit);
        }
        ip += lit; op += lit;
        if (ip >= iend) break;                     /* last sequence: literals only */

        if (iend - ip < 2) return -1;
        off = (size_t)ip[0] | ((size_t)ip[1] << 8); ip += 2;
        if (off == 0 || off > (size_t)(op - dst)) return -1;

        ml = token & 15;
        if (ml == 15) {
            unsigned b;
            do { if (ip >= iend) return -1; b = *ip++; ml += b; } while (b == 255);
        }
        ml += LZ4_MINMATCH;
        if ((size_t)(oend - op) < ml) return -1;

        ref = op - off;
        if (off >= 8 && ml <= 16 && oend - op >= 16) {
            /* short match: two 8-byte chunks, each non-overlapping (off >= 8) */
            memcpy(op, ref, 8); memcpy(op + 8, ref + 8, 8);
            op += ml;
        } else if (off >= ml) {
            memcpy(op, ref, ml);
            op += ml;
        } else {
            /* overlapping (e.g. a run of identical pixels, off == bpp):
               replicate with doubling chunks instead of byte-at-a-time */
            while (ml) {
                size_t c = (size_t)(op - ref);
                if (c > ml) c = ml;
                memcpy(op, ref, c);
                op += c; ml -= c;
            }
        }
    }
    return op == oend ? 0 : -1;
}

/* ======================================================================
 * Pixel conversion
 * ==================================================================== */

void g4_bgra_to_wire32(const uint8_t *s, uint8_t *d, size_t npix)
{
    size_t i;
    for (i = 0; i < npix; i++, s += 4, d += 4) {
        d[0] = s[3]; d[1] = s[2]; d[2] = s[1]; d[3] = s[0];
    }
}

/* ---------- 4:2:2 YCbCr (full range), exact integer maths ----------
   Every shift is applied to a non-negative value, so encoder (M1) and
   decoder (PowerPC) get bit-identical results. */
static inline uint8_t clamp255(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

void g4_yuv_from_pair(const uint8_t *a, const uint8_t *b, uint8_t *o)
{
    int r0 = a[1], g0 = a[2], b0 = a[3], r1 = b[1], g1 = b[2], b1 = b[3];
    int ra = (r0 + r1 + 1) >> 1, ga = (g0 + g1 + 1) >> 1, ba = (b0 + b1 + 1) >> 1;
    o[1] = clamp255((77 * r0 + 150 * g0 + 29 * b0 + 128) >> 8);                    /* Y0 */
    o[3] = clamp255((77 * r1 + 150 * g1 + 29 * b1 + 128) >> 8);                    /* Y1 */
    o[0] = clamp255((128 * ba - 43 * ra - 85 * ga + 32896) >> 8);                  /* Cb */
    o[2] = clamp255((128 * ra - 107 * ga - 21 * ba + 32896) >> 8);                 /* Cr */
}

void g4_yuv_to_pair(const uint8_t *in, uint8_t *a, uint8_t *b)
{
    int c = in[0] - 128, d = in[2] - 128;
    int rd = ((359 * d + 128 + 46080) >> 8) - 180;
    int gd = ((88 * c + 183 * d + 128 + 34816) >> 8) - 136;
    int bd = ((454 * c + 128 + 58112) >> 8) - 227;
    int y0 = in[1], y1 = in[3];
    a[0] = 0xFF; a[1] = clamp255(y0 + rd); a[2] = clamp255(y0 - gd); a[3] = clamp255(y0 + bd);
    b[0] = 0xFF; b[1] = clamp255(y1 + rd); b[2] = clamp255(y1 - gd); b[3] = clamp255(y1 + bd);
}

/* One row of 4:2:2 (Cb,Y0,Cr,Y1 per pair) to A,R,G,B, w pixels. On G4/G5
   builds the AltiVec version in g4yuv_altivec.c does 8 pixels at a time;
   it is bit-identical to g4_yuv_to_pair (tested exhaustively). It lives in
   its own file, compiled only when G4_HAVE_ALTIVEC is defined, so that a
   compiler that can't build it (Tiger's gcc 4.0 has run out of memory on
   it) still builds everything else. */
#if defined(G4_HAVE_ALTIVEC)
void g4_yuv8_altivec(const uint8_t *in, uint8_t *out);
#endif

/* AltiVec can be switched off at run time to compare speeds (the app's
   Performance menu); the output is the same either way */
static volatile int yuv_simd_on = 1;
int  g4_yuv_simd_available(void)
{
#if defined(G4_HAVE_ALTIVEC)
    return 1;
#else
    return 0;
#endif
}
void g4_yuv_set_simd(int on) { yuv_simd_on = on != 0; }
int  g4_yuv_get_simd(void) { return g4_yuv_simd_available() && yuv_simd_on; }

void g4_yuv_row(const uint8_t *in, uint8_t *out, unsigned w)
{
    unsigned x = 0;
    uint8_t tmp[4];
#if defined(G4_HAVE_ALTIVEC)
    if (yuv_simd_on)
        for (; x + 8 <= w; x += 8) g4_yuv8_altivec(in + 2 * x, out + 4 * x);
#endif
    for (; x + 1 < w; x += 2) g4_yuv_to_pair(in + 2 * x, out + 4 * x, out + 4 * x + 4);
    if (x < w) g4_yuv_to_pair(in + 2 * x, out + 4 * x, tmp);
}

/* ---- half-resolution video ---- */

/* average of two A,R,G,B pixels, byte by byte, rounding up (any byte order) */
static uint32_t avg_px(uint32_t a, uint32_t b)
{
    return (a | b) - (((a ^ b) >> 1) & 0x7F7F7F7Fu);
}
static uint32_t ld32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static void st32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

/* One output row from half-res rows a and b (b == a for the even rows):
   pixel 2i is avg(a[i], b[i]), pixel 2i+1 also averages with column i+1. */
static void yuvh_row_out(const uint8_t *a, const uint8_t *b, unsigned hw, uint8_t *out, unsigned w)
{
    unsigned i;
    uint32_t cur = a == b ? ld32(a) : avg_px(ld32(a), ld32(b));
    for (i = 0; i < hw; i++) {
        uint32_t nxt = cur;
        if (i + 1 < hw) nxt = a == b ? ld32(a + 4 * i + 4) : avg_px(ld32(a + 4 * i + 4), ld32(b + 4 * i + 4));
        st32(out + 8 * i, cur);
        if (2 * i + 1 < w) st32(out + 8 * i + 4, avg_px(cur, nxt));
        cur = nxt;
    }
}

void g4_yuvh_paint(const uint8_t *src, unsigned w, unsigned h, uint8_t *dst, size_t stride, uint8_t *rows)
{
    unsigned hw = (w + 1) / 2, hh = (h + 1) / 2, pairs = (hw + 1) / 2, j;
    size_t rowy = (size_t)pairs * 4;
    uint8_t *a = rows, *b = rows + 4 * (size_t)hw, *t;
    if (!w || !h) return;
    g4_yuv_row(src, a, hw);
    for (j = 0; j < hh; j++) {
        int more = j + 1 < hh;
        if (more) g4_yuv_row(src + (size_t)(j + 1) * rowy, b, hw);
        yuvh_row_out(a, a, hw, dst + (size_t)(2 * j) * stride, w);
        if (2 * j + 1 < h) yuvh_row_out(a, more ? b : a, hw, dst + (size_t)(2 * j + 1) * stride, w);
        t = a; a = b; b = t;
    }
}

/* ======================================================================
 * Encoder
 * ==================================================================== */

#define TILE        G4_TILE
#define SCROLL_BAND 32      /* column band width for scroll matching (pixels) */
#define SCROLL_MAXB 192     /* bands per search: screens up to 6144 pixels wide */
#define MAX_HINTS   64

typedef struct { int x, y, w, h, c0, c1, open; } mrect;

struct g4_encoder {
    int w, h, depth, bpp;
    size_t stride;
    uint8_t *cur, *model;
    int force_full;

    int nhints;
    int hints[MAX_HINTS][4];

    int tw, th;
    uint8_t *tiles;
    mrect *rects;

    uint8_t *out;    size_t out_cap, out_len;
    uint8_t *gather; size_t gather_cap;
    uint8_t *lz;     size_t lz_cap;

    uint64_t *hc, *hm;     /* per row and column band: segment hashes (current / model) */
    uint8_t  *uc, *um;     /* segment is a single colour                               */
    int       nbands;      /* column bands per row (SCROLL_BAND pixels each)           */
    /* moves in any direction (window drags, sideways scrolling) */
    uint32_t *mkey; int32_t *mpos; size_t mcap;      /* sampled current segments   */
    uint32_t *vkey; int32_t *vcnt; size_t vcap;      /* votes per (dx, dy)         */
    int32_t  *hist;                                   /* largest-rectangle scratch  */
    int32_t  *votes;       /* indexed dy + h                       */
    uint64_t *tkey;        /* row hash table for scroll matching   */
    int32_t  *tval;
    size_t    tcap;

    /* tile cache: a mirror of the client's cache, kept exact on our side so
       a hash collision can never put wrong pixels on the G4 */
    int       slots;
    uint8_t  *cpx;         /* slots * tile bytes                   */
    uint64_t *chash;
    uint8_t  *cvalid, *cref;
    int       chand;       /* CLOCK replacement hand               */
    int32_t  *ctab;        /* hash -> slot, open addressing        */
    size_t    ctab_cap, ctab_used;
    uint64_t *thash;       /* per screen tile: hash this frame     */
    uint8_t  *tstate;      /* per screen tile: 1 hashed, 2 uniform */
    uint32_t *tlast;       /* per screen tile: time it last changed */
    uint8_t  *tvol;        /* per screen tile: changing at video rate */
    uint32_t  now_ms;
    int       clock_set;

    /* video tiles sent as 4:2:2: the model holds what the client shows (the
       lossy version); lhash remembers the exact source so the still tile
       isn't resent every frame, and it is resent losslessly once still */
    int       yuv;
    int       yuvh;      /* video at half resolution (G4_RECT_YUVH) */
    int       pal;       /* few-colour areas as palette + indices (G4_RECT_PAL) */
    uint8_t  *pbuf;      size_t pbuf_cap;     /* palette index stream */
    uint8_t  *plz;       size_t plz_cap;      /* ...LZ4-compressed    */
    uint8_t  *hrows;     /* two half-res rows for g4_yuvh_paint */
    size_t    hrows_cap;
    uint8_t  *tfast;       /* per tile: changed again within YUV_FAST_MS */
    uint8_t  *lossy;
    uint64_t *lhash;
    uint32_t *llast;
    uint8_t  *refresh;     /* per tile: changed only because it's due a lossless refresh */
    uint8_t  *vmask;

    /* which tiles to compare on the next encode (dirty-rect ingestion) */
    uint8_t  *dmask;
    int       all_dirty;
};

#define YUV_FAST_MS     250   /* changed again this soon: treat as video   */
#define YUV_REFRESH_MS  300   /* still this long: resend losslessly        */
#define YUV_MIN_TILES   4     /* smaller animations stay lossless          */

#define TILE_BYTES(e) ((size_t)TILE * TILE * (e)->bpp)

static int ensure(uint8_t **buf, size_t *cap, size_t need)
{
    if (need <= *cap) return 0;
    {
        size_t nc = *cap ? *cap : 65536;
        uint8_t *nb;
        while (nc < need) nc *= 2;
        nb = (uint8_t *)realloc(*buf, nc);
        if (!nb) return -1;
        *buf = nb; *cap = nc;
    }
    return 0;
}

g4_encoder *g4enc_create(int width, int height, int depth)
{
    g4_encoder *e;
    size_t fbsize;
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096) return NULL;
    if (depth != 16 && depth != 32) return NULL;

    e = (g4_encoder *)calloc(1, sizeof *e);
    if (!e) return NULL;
    e->w = width; e->h = height; e->depth = depth;
    e->bpp = depth == 16 ? 2 : 4;
    e->stride = (size_t)width * e->bpp;
    fbsize = e->stride * height;
    e->tw = (width + TILE - 1) / TILE;
    e->th = (height + TILE - 1) / TILE;

    e->cur   = (uint8_t *)calloc(1, fbsize);
    e->model = (uint8_t *)calloc(1, fbsize);
    e->tiles = (uint8_t *)calloc((size_t)e->tw * e->th, 1);
    e->rects = (mrect *)calloc((size_t)e->tw * e->th, sizeof(mrect));
    e->nbands = (width + SCROLL_BAND - 1) / SCROLL_BAND;
    e->hc = (uint64_t *)calloc((size_t)height * e->nbands, sizeof(uint64_t));
    e->hm = (uint64_t *)calloc((size_t)height * e->nbands, sizeof(uint64_t));
    e->uc = (uint8_t *)calloc((size_t)height * e->nbands, 1);
    e->um = (uint8_t *)calloc((size_t)height * e->nbands, 1);
    e->votes = (int32_t *)calloc((size_t)2 * height + 1, sizeof(int32_t));
    e->tcap = 1; while (e->tcap < (size_t)height * e->nbands * 2) e->tcap <<= 1;
    e->tkey = (uint64_t *)calloc(e->tcap, sizeof(uint64_t));
    e->tval = (int32_t *)calloc(e->tcap, sizeof(int32_t));
    e->mcap = 1 << 15; e->vcap = 1 << 12;
    e->mkey = (uint32_t *)calloc(e->mcap, sizeof(uint32_t));
    e->mpos = (int32_t *)calloc(e->mcap, sizeof(int32_t));
    e->vkey = (uint32_t *)calloc(e->vcap, sizeof(uint32_t));
    e->vcnt = (int32_t *)calloc(e->vcap, sizeof(int32_t));
    e->hist = (int32_t *)calloc((size_t)(width + TILE - 1) / TILE + 1, sizeof(int32_t) * 2);
    {
        size_t nt = (size_t)e->tw * e->th;
        e->tlast = (uint32_t *)calloc(nt, sizeof(uint32_t));
        e->tvol = (uint8_t *)calloc(nt, 1);
        e->tfast = (uint8_t *)calloc(nt, 1);
        e->lossy = (uint8_t *)calloc(nt, 1);
        e->lhash = (uint64_t *)calloc(nt, sizeof(uint64_t));
        e->llast = (uint32_t *)calloc(nt, sizeof(uint32_t));
        e->refresh = (uint8_t *)calloc(nt, 1);
        e->vmask = (uint8_t *)calloc(nt, 1);
        e->dmask = (uint8_t *)calloc(nt, 1);
    }
    e->all_dirty = 1;

    if (!e->cur || !e->model || !e->tiles || !e->rects || !e->hc || !e->hm ||
        !e->uc || !e->um || !e->votes || !e->tkey || !e->tval ||
        !e->mkey || !e->mpos || !e->vkey || !e->vcnt || !e->hist || !e->tlast || !e->tvol ||
        !e->tfast || !e->lossy || !e->lhash || !e->llast || !e->refresh || !e->vmask || !e->dmask) {
        g4enc_destroy(e);
        return NULL;
    }
    e->force_full = 1;
    return e;
}

void g4enc_destroy(g4_encoder *e)
{
    if (!e) return;
    free(e->cur); free(e->model); free(e->tiles); free(e->rects);
    free(e->out); free(e->gather); free(e->lz);
    free(e->hc); free(e->hm); free(e->uc); free(e->um); free(e->votes);
    free(e->tkey); free(e->tval); free(e->hrows);
    free(e->pbuf); free(e->plz);
    free(e->mkey); free(e->mpos); free(e->vkey); free(e->vcnt); free(e->hist);
    free(e->cpx); free(e->chash); free(e->cvalid); free(e->cref); free(e->ctab);
    free(e->thash); free(e->tstate); free(e->tlast); free(e->tvol);
    free(e->tfast); free(e->lossy); free(e->lhash); free(e->llast); free(e->refresh); free(e->vmask); free(e->dmask);
    free(e);
}

static void cache_clear(g4_encoder *e)
{
    size_t i;
    if (!e->slots) return;
    memset(e->cvalid, 0, (size_t)e->slots);
    memset(e->cref, 0, (size_t)e->slots);
    for (i = 0; i < e->ctab_cap; i++) e->ctab[i] = -1;
    e->ctab_used = 0;
    e->chand = 0;
}

void g4enc_set_time(g4_encoder *e, uint32_t ms) { e->now_ms = ms; e->clock_set = 1; }

void g4enc_set_yuv(g4_encoder *e, int on) { e->yuv = on && e->depth == 32; }
void g4enc_set_yuv_half(g4_encoder *e, int on) { e->yuvh = on != 0; }
void g4enc_set_palette(g4_encoder *e, int on) { e->pal = on != 0; }

int g4enc_refresh_due(const g4_encoder *e, uint32_t now_ms)
{
    int i, n = e->tw * e->th, best = -1;
    for (i = 0; i < n; i++) {
        int left;
        if (!e->lossy[i]) continue;
        left = YUV_REFRESH_MS - (int)(uint32_t)(now_ms - e->llast[i]);
        if (left < 0) left = 0;
        if (best < 0 || left < best) best = left;
    }
    return best;
}

void g4enc_force_full(g4_encoder *e)
{
    e->force_full = 1;
    memset(e->lossy, 0, (size_t)e->tw * e->th);
    cache_clear(e);   /* new connection: the client's cache is empty */
}

int g4enc_enable_cache(g4_encoder *e, int slots)
{
    size_t ntiles = (size_t)e->tw * e->th;
    free(e->cpx); free(e->chash); free(e->cvalid); free(e->cref); free(e->ctab);
    free(e->thash); free(e->tstate);
    e->cpx = NULL; e->chash = NULL; e->cvalid = e->cref = NULL; e->ctab = NULL;
    e->thash = NULL; e->tstate = NULL;
    e->slots = 0;
    if (slots <= 0) return 0;
    if (slots > G4_MAX_SLOTS) slots = G4_MAX_SLOTS;

    e->ctab_cap = 1; while (e->ctab_cap < (size_t)slots * 4) e->ctab_cap <<= 1;
    e->cpx = (uint8_t *)malloc((size_t)slots * TILE_BYTES(e));
    e->chash = (uint64_t *)calloc((size_t)slots, sizeof(uint64_t));
    e->cvalid = (uint8_t *)calloc((size_t)slots, 1);
    e->cref = (uint8_t *)calloc((size_t)slots, 1);
    e->ctab = (int32_t *)malloc(e->ctab_cap * sizeof(int32_t));
    e->thash = (uint64_t *)calloc(ntiles, sizeof(uint64_t));
    e->tstate = (uint8_t *)calloc(ntiles, 1);
    if (!e->cpx || !e->chash || !e->cvalid || !e->cref || !e->ctab || !e->thash || !e->tstate) {
        g4enc_enable_cache(e, 0);
        return -1;
    }
    e->slots = slots;
    cache_clear(e);
    return 0;
}
const uint8_t *g4enc_current(const g4_encoder *e) { return e->cur; }
const uint8_t *g4enc_model(const g4_encoder *e) { return e->model; }

static void ingest_rows(g4_encoder *e, const uint8_t *src, size_t sstride, int x0, int y0, int w, int h)
{
    int y, x;
    for (y = y0; y < y0 + h; y++) {
        const uint8_t *s = src + (size_t)y * sstride + (size_t)x0 * 4;
        uint8_t *d = e->cur + (size_t)y * e->stride + (size_t)x0 * e->bpp;
        if (e->depth == 32) {
            g4_bgra_to_wire32(s, d, (size_t)w);
        } else {
            for (x = 0; x < w; x++, s += 4, d += 2) {
                uint32_t v = 0x8000u | ((uint32_t)(s[2] >> 3) << 10) |
                             ((uint32_t)(s[1] >> 3) << 5) | (uint32_t)(s[0] >> 3);
                d[0] = (uint8_t)(v >> 8); d[1] = (uint8_t)v;
            }
        }
    }
}

void g4enc_ingest_bgra(g4_encoder *e, const uint8_t *src, size_t sstride)
{
    ingest_rows(e, src, sstride, 0, 0, e->w, e->h);
    e->all_dirty = 1;
}

void g4enc_ingest_bgra_rect(g4_encoder *e, const uint8_t *src, size_t sstride, int x, int y, int w, int h)
{
    int tx, ty;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > e->w) w = e->w - x;
    if (y + h > e->h) h = e->h - y;
    if (w <= 0 || h <= 0) return;
    ingest_rows(e, src, sstride, x, y, w, h);
    for (ty = y / TILE; ty * TILE < y + h; ty++)
        for (tx = x / TILE; tx * TILE < x + w; tx++) e->dmask[ty * e->tw + tx] = 1;
}

void g4enc_add_hint(g4_encoder *e, int x, int y, int w, int h)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > e->w) w = e->w - x;
    if (y + h > e->h) h = e->h - y;
    if (w <= 0 || h <= 0) return;
    if (e->nhints == MAX_HINTS) {
        /* too many: collapse everything into one bounding box */
        int i, x0 = x, y0 = y, x1 = x + w, y1 = y + h;
        for (i = 0; i < e->nhints; i++) {
            int *r = e->hints[i];
            if (r[0] < x0) x0 = r[0];
            if (r[1] < y0) y0 = r[1];
            if (r[0] + r[2] > x1) x1 = r[0] + r[2];
            if (r[1] + r[3] > y1) y1 = r[1] + r[3];
        }
        e->nhints = 1;
        e->hints[0][0] = x0; e->hints[0][1] = y0;
        e->hints[0][2] = x1 - x0; e->hints[0][3] = y1 - y0;
        return;
    }
    e->hints[e->nhints][0] = x; e->hints[e->nhints][1] = y;
    e->hints[e->nhints][2] = w; e->hints[e->nhints][3] = h;
    e->nhints++;
}

/* ---------- scroll detection ---------- */

static uint64_t hash_seg(const uint8_t *p, size_t n)
{
    uint64_t h = 0x9E3779B97F4A7C15ull ^ (uint64_t)n;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w; memcpy(&w, p + i, 8);
        h ^= w; h *= 0xff51afd7ed558ccdull; h ^= h >> 32;
    }
    for (; i < n; i++) { h ^= p[i]; h *= 0x100000001b3ull; }
    return h;
}

static void move_rows(uint8_t *fb, size_t stride, size_t xoff, size_t rowbytes,
                      int sy, int dy, int h)
{
    int i;
    if (dy < sy) {
        for (i = 0; i < h; i++)
            memmove(fb + (size_t)(dy + i) * stride + xoff, fb + (size_t)(sy + i) * stride + xoff, rowbytes);
    } else {
        for (i = h - 1; i >= 0; i--)
            memmove(fb + (size_t)(dy + i) * stride + xoff, fb + (size_t)(sy + i) * stride + xoff, rowbytes);
    }
}

/*
 * Look for a vertical scroll inside rect R: content that sat at model row
 * y+dy now sits at current row y. On success apply the copy to the model and
 * append a COPY rect. Returns 1 if a copy was emitted.
 *
 * Rows are compared in column bands of SCROLL_BAND pixels, not whole: the
 * area searched often takes in things that don't move with the page (the
 * wallpaper or a neighbouring window at the edge of the changed area, a
 * sidebar, an overlay scrollbar), and those would make every whole row
 * differ. The copy covers the widest run of bands that moved together; what
 * it gets wrong is caught by the tile diff that follows, so it stays exact.
 */
static int detect_scroll(g4_encoder *e, int rx, int ry, int rw, int rh,
                         uint8_t **wp, int *nrects, g4_enc_stats *st)
{
    const int NB = e->nbands;
    int nb, y, b, i, changed = 0, best_dy = 0, best_votes = 0;
    int b0, b1, run0 = -1, runlen = 0, best_area = 0, cx = 0, cw = 0;
    size_t mask = e->tcap - 1;
    int good[SCROLL_MAXB], tot[SCROLL_MAXB];   /* tot: rows that stayed in place */

    if (rw < 64 || rh < 48) return 0;
    nb = (rw + SCROLL_BAND - 1) / SCROLL_BAND;
    if (nb > NB || nb > (int)(sizeof good / sizeof good[0])) return 0;

#define SEG(y_, b_) ((size_t)(y_) * NB + (b_))
    for (y = ry; y < ry + rh; y++) {
        const uint8_t *c = e->cur + (size_t)y * e->stride + (size_t)rx * e->bpp;
        const uint8_t *m = e->model + (size_t)y * e->stride + (size_t)rx * e->bpp;
        for (b = 0; b < nb; b++) {
            int x0 = b * SCROLL_BAND, w = rw - x0 < SCROLL_BAND ? rw - x0 : SCROLL_BAND;
            size_t off = (size_t)x0 * e->bpp, n = (size_t)w * e->bpp, k = SEG(y, b);
            e->hc[k] = hash_seg(c + off, n) ^ ((uint64_t)b * 0x9E3779B97F4A7C15ull);
            e->hm[k] = hash_seg(m + off, n) ^ ((uint64_t)b * 0x9E3779B97F4A7C15ull);
            e->uc[k] = w < 2 || memcmp(c + off, c + off + e->bpp, n - e->bpp) == 0;
            e->um[k] = w < 2 || memcmp(m + off, m + off + e->bpp, n - e->bpp) == 0;
        }
    }

    /* table: (band, model hash) -> first row with it (non-uniform segments only) */
    for (i = 0; i < (int)e->tcap; i++) e->tval[i] = -1;
    for (y = ry; y < ry + rh; y++)
        for (b = 0; b < nb; b++) {
            size_t k, s = SEG(y, b);
            if (e->um[s]) continue;
            k = (size_t)(e->hm[s] ^ (e->hm[s] >> 29)) & mask;
            while (e->tval[k] >= 0 && e->tkey[k] != e->hm[s]) k = (k + 1) & mask;
            if (e->tval[k] < 0) { e->tkey[k] = e->hm[s]; e->tval[k] = y; }
        }

    /* every changed, non-uniform segment votes for where it came from */
    memset(e->votes + (e->h - rh), 0, sizeof(int32_t) * (size_t)(2 * rh + 1));
    for (y = ry; y < ry + rh; y++)
        for (b = 0; b < nb; b++) {
            size_t k, s = SEG(y, b);
            if (e->uc[s] || e->hc[s] == e->hm[s]) continue;     /* blank, or unchanged in place */
            changed++;
            k = (size_t)(e->hc[s] ^ (e->hc[s] >> 29)) & mask;
            while (e->tval[k] >= 0) {
                if (e->tkey[k] == e->hc[s]) { e->votes[e->h + (e->tval[k] - y)]++; break; }
                k = (k + 1) & mask;
            }
        }
    for (i = -rh + 1; i < rh; i++) {
        if (i == 0) continue;
        if (e->votes[e->h + i] > best_votes) { best_votes = e->votes[e->h + i]; best_dy = i; }
    }
    if (best_dy == 0 || best_votes < 8 || best_votes * 4 < changed) return 0;

    /* Which bands moved by best_dy: rows that match there (good) against
       rows that stayed put instead (stat: wallpaper, a sidebar, another
       window). Rows that match neither (new content scrolling in) don't
       count either way. */
    for (b = 0; b < nb; b++) {
        good[b] = tot[b] = 0;
        for (y = ry; y < ry + rh; y++) {
            int sy = y + best_dy;
            size_t s = SEG(y, b);
            if (e->uc[s]) continue;
            if (sy >= ry && sy < ry + rh && e->hc[s] == e->hm[SEG(sy, b)]) good[b]++;
            else if (e->hc[s] == e->hm[s]) tot[b]++;
        }
    }

    /* The core: the run of bands that clearly moved (more rows matching at
       best_dy than staying put) with the most matching rows. Then the
       longest run of rows where all of it matches, trimming up to two edge
       bands (a scrollbar) when that gives a bigger copy. */
    {
        int c0 = -1, c1 = -1, bestgood = 0, lo, hi;
        for (b0 = 0; b0 < nb; ) {
            int sum = 0;
            if (good[b0] <= tot[b0]) { b0++; continue; }
            for (b1 = b0; b1 < nb && good[b1] > tot[b1]; b1++) sum += good[b1];
            if (sum > bestgood) { bestgood = sum; c0 = b0; c1 = b1; }
            b0 = b1;
        }
        if (c0 < 0) return 0;
        for (lo = c0; lo < c1 && lo <= c0 + 2; lo++)
            for (hi = c1; hi > lo && hi >= c1 - 2; hi--) {
                int cur0 = -1, curlen = 0, r0 = -1, rl = 0, area;
                for (y = ry; y < ry + rh; y++) {
                    int sy = y + best_dy, ok = sy >= ry && sy < ry + rh;
                    for (b = lo; ok && b < hi; b++) ok = e->hc[SEG(y, b)] == e->hm[SEG(sy, b)];
                    if (ok) {
                        if (cur0 < 0) { cur0 = y; curlen = 0; }
                        if (++curlen > rl) { rl = curlen; r0 = cur0; }
                    } else cur0 = -1;
                }
                area = rl * (hi - lo);
                if (rl >= 16 && area > best_area) { best_area = area; run0 = r0; runlen = rl; cx = lo; cw = hi - lo; }
            }
        if (best_area == 0) return 0;
        /* widen over neighbouring bands that match on every row of the run
           (blank margins, sparse text, more of the same page) */
        for (;;) {
            int grew = 0, side;
            for (side = 0; side < 2; side++) {
                int nbnd = side ? cx + cw : cx - 1, ok = nbnd >= 0 && nbnd < nb;
                for (y = run0; ok && y < run0 + runlen; y++)
                    ok = e->hc[SEG(y, nbnd)] == e->hm[SEG(y + best_dy, nbnd)];
                if (ok) { if (side) cw++; else { cx--; cw++; } grew = 1; }
            }
            if (!grew) break;
        }
    }
#undef SEG
    if (best_area == 0 || cw * SCROLL_BAND < 64) return 0;
    {
        int x = rx + cx * SCROLL_BAND;
        int w = cx * SCROLL_BAND + cw * SCROLL_BAND > rw ? rw - cx * SCROLL_BAND : cw * SCROLL_BAND;
        size_t xoff = (size_t)x * e->bpp, seg = (size_t)w * e->bpp;
        size_t pos = (size_t)(*wp - e->out);
        uint8_t *p;
        if (ensure(&e->out, &e->out_cap, pos + 14)) return 0;   /* may move e->out */
        p = e->out + pos;
        p[0] = G4_RECT_COPY; p[1] = 0;
        g4_put16(p + 2, (uint32_t)x);  g4_put16(p + 4, (uint32_t)run0);
        g4_put16(p + 6, (uint32_t)w);  g4_put16(p + 8, (uint32_t)runlen);
        g4_put16(p + 10, (uint32_t)x); g4_put16(p + 12, (uint32_t)(run0 + best_dy));
        *wp = p + 14;
        move_rows(e->model, e->stride, xoff, seg, run0 + best_dy, run0, runlen);
    }
    memset(e->lossy, 0, (size_t)e->tw * e->th);   /* lossy pixels moved: compare exactly from now on */
    (*nrects)++;
    if (st) { st->copies++; st->rects++; st->scroll_dy = best_dy; }
    return 1;
}

/* ---------- moves in any direction ---------- */

#define MOVE_SEG 16          /* pixels per sampled segment */

static uint32_t px_at(const uint8_t *p, int bpp)
{
    return bpp == 4 ? ((uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3])
                    : ((uint32_t)p[0] << 8 | p[1]);
}

/* polynomial hash of MOVE_SEG pixels; the model scan rolls the same hash */
#define MOVE_BASE 0x01000193u
static uint32_t seg_hash(const uint8_t *p, int bpp)
{
    uint32_t h = 0;
    int i;
    for (i = 0; i < MOVE_SEG; i++) h = h * MOVE_BASE + px_at(p + i * bpp, bpp) + 1;
    return h;
}

/* copy a w x h block from (sx, sy) to (dx, dy) in fb, overlap-safe; the
   decoder's G4_RECT_COPY does exactly the same */
static void move_block(uint8_t *fb, size_t stride, int bpp, int sx, int sy, int dx, int dy, int w, int h)
{
    size_t n = (size_t)w * bpp;
    int r;
    if (dy <= sy) for (r = 0; r < h; r++)
        memmove(fb + (size_t)(dy + r) * stride + (size_t)dx * bpp, fb + (size_t)(sy + r) * stride + (size_t)sx * bpp, n);
    else for (r = h - 1; r >= 0; r--)
        memmove(fb + (size_t)(dy + r) * stride + (size_t)dx * bpp, fb + (size_t)(sy + r) * stride + (size_t)sx * bpp, n);
}

/*
 * Content that moved by (mx, my) in any direction inside rect R (a window
 * being dragged, a sideways scroll): current pixel (x, y) == model pixel
 * (x + mx, y + my). Samples 16-pixel segments of what changed, finds them in
 * the previous frame with a rolling hash, takes the most common offset, and
 * copies the largest block of 32x32 tiles that moved by exactly that much.
 * The tile diff afterwards fixes anything else, so this never costs
 * correctness. Returns 1 if a copy was emitted.
 */
static int detect_move(g4_encoder *e, int rx, int ry, int rw, int rh,
                       uint8_t **wp, int *nrects, g4_enc_stats *st)
{
    const int bpp = e->bpp;
    size_t mmask = e->mcap - 1, vmask = e->vcap - 1, i;
    int x, y, nsamp = 0, best = 0, bmx = 0, bmy = 0, nvotes = 0;
    uint32_t pw = 1;

    if (rw < 64 || rh < 32) return 0;
    memset(e->mpos, 0xFF, e->mcap * sizeof(int32_t));
    memset(e->vcnt, 0, e->vcap * sizeof(int32_t));

    /* sample changed, non-blank segments of the current frame */
    for (y = ry; y < ry + rh && nsamp < (int)(e->mcap / 2); y += 4)
        for (x = rx; x + MOVE_SEG <= rx + rw && nsamp < (int)(e->mcap / 2); x += MOVE_SEG) {
            const uint8_t *c = e->cur + (size_t)y * e->stride + (size_t)x * bpp;
            const uint8_t *m = e->model + (size_t)y * e->stride + (size_t)x * bpp;
            size_t n = (size_t)MOVE_SEG * bpp, k;
            uint32_t h;
            if (!memcmp(c, m, n) || !memcmp(c, c + bpp, n - bpp)) continue;   /* unchanged or blank */
            h = seg_hash(c, bpp);
            k = (h ^ (h >> 15)) & mmask;
            while (e->mpos[k] >= 0 && e->mkey[k] != h) k = (k + 1) & mmask;
            if (e->mpos[k] >= 0) continue;                 /* repeated content: ambiguous */
            e->mkey[k] = h; e->mpos[k] = y * e->w + x;
            nsamp++;
        }
    if (nsamp < 8) return 0;

    /* roll the hash along every model row of R; each hit votes for an offset */
    for (i = 1; i < MOVE_SEG; i++) pw *= MOVE_BASE;
    for (y = ry; y < ry + rh; y++) {
        const uint8_t *m = e->model + (size_t)y * e->stride;
        uint32_t h = seg_hash(m + (size_t)rx * bpp, bpp);
        for (x = rx; x + MOVE_SEG <= rx + rw; x++) {
            size_t k = (h ^ (h >> 15)) & mmask;
            while (e->mpos[k] >= 0) {
                if (e->mkey[k] == h) {
                    int cx = e->mpos[k] % e->w, cy = e->mpos[k] / e->w;
                    int mx = x - cx, my = y - cy;
                    if ((mx || my) && !memcmp(e->cur + (size_t)cy * e->stride + (size_t)cx * bpp,
                                             m + (size_t)x * bpp, (size_t)MOVE_SEG * bpp)) {
                        uint32_t key = ((uint32_t)(mx + 32768) << 16) | (uint32_t)(my + 32768);
                        size_t v = (key * 2654435761u >> 8) & vmask;
                        while (e->vcnt[v] && e->vkey[v] != key) v = (v + 1) & vmask;
                        /* a new offset only while the table has room (repetitive
                           content can match at thousands of offsets) */
                        if (e->vcnt[v] || nvotes < (int)(e->vcap * 3 / 4)) {
                            if (!e->vcnt[v]) { e->vkey[v] = key; nvotes++; }
                            if (++e->vcnt[v] > best) { best = e->vcnt[v]; bmx = mx; bmy = my; }
                        }
                    }
                    break;
                }
                k = (k + 1) & mmask;
            }
            if (x + MOVE_SEG < rx + rw)        /* roll: drop pixel x, add pixel x + MOVE_SEG */
                h = (h - (px_at(m + (size_t)x * bpp, bpp) + 1) * pw) * MOVE_BASE
                    + px_at(m + (size_t)(x + MOVE_SEG) * bpp, bpp) + 1;
        }
    }
    if (best < 8 || best * 5 < nsamp) return 0;

    /* which tiles of R moved by exactly (bmx, bmy); then the largest
       rectangle of them (histogram method, one pass over tile rows) */
    {
        int tx0 = rx / TILE, ty0 = ry / TILE, tx1 = (rx + rw + TILE - 1) / TILE, ty1 = (ry + rh + TILE - 1) / TILE;
        int ntx = tx1 - tx0, tx, ty, bestA = 0, R0x = 0, R0y = 0, R1x = 0, R1y = 0;
        int32_t *hgt = e->hist, *stk = e->hist + ntx + 1;
        if (tx1 > e->tw) tx1 = e->tw;
        if (ty1 > e->th) ty1 = e->th;
        ntx = tx1 - tx0;
        memset(hgt, 0, sizeof(int32_t) * (size_t)(ntx + 1));
        for (ty = ty0; ty < ty1; ty++) {
            int top = 0;
            for (tx = tx0; tx < tx1; tx++) {
                int X0 = tx * TILE, Y0 = ty * TILE, X1 = X0 + TILE, Y1 = Y0 + TILE, ok, r;
                if (X1 > e->w) X1 = e->w;
                if (Y1 > e->h) Y1 = e->h;
                ok = X0 + bmx >= 0 && X1 + bmx <= e->w && Y0 + bmy >= 0 && Y1 + bmy <= e->h &&
                     e->tiles[ty * e->tw + tx];
                for (r = Y0; ok && r < Y1; r++)
                    ok = !memcmp(e->cur + (size_t)r * e->stride + (size_t)X0 * bpp,
                                 e->model + (size_t)(r + bmy) * e->stride + (size_t)(X0 + bmx) * bpp,
                                 (size_t)(X1 - X0) * bpp);
                hgt[tx - tx0] = ok ? hgt[tx - tx0] + 1 : 0;
            }
            hgt[ntx] = 0;
            for (tx = 0; tx <= ntx; tx++) {            /* largest rectangle in the histogram */
                int start = tx;
                while (top > 0 && hgt[stk[top - 1]] >= hgt[tx]) {
                    int j = stk[--top], left = top > 0 ? stk[top - 1] + 1 : 0, a = hgt[j] * (tx - left);
                    if (a > bestA) { bestA = a; R0x = tx0 + left; R1x = tx0 + tx; R1y = ty + 1; R0y = ty + 1 - hgt[j]; }
                    start = j;
                }
                (void)start;
                stk[top++] = tx;
            }
        }
        if (bestA < 4) return 0;
        {
            int X = R0x * TILE, Y = R0y * TILE;
            int W = (R1x * TILE > e->w ? e->w : R1x * TILE) - X, H = (R1y * TILE > e->h ? e->h : R1y * TILE) - Y;
            size_t pos = (size_t)(*wp - e->out);
            uint8_t *p;
            if (ensure(&e->out, &e->out_cap, pos + 14)) return 0;
            p = e->out + pos;
            p[0] = G4_RECT_COPY; p[1] = 0;
            g4_put16(p + 2, (uint32_t)X);         g4_put16(p + 4, (uint32_t)Y);
            g4_put16(p + 6, (uint32_t)W);         g4_put16(p + 8, (uint32_t)H);
            g4_put16(p + 10, (uint32_t)(X + bmx)); g4_put16(p + 12, (uint32_t)(Y + bmy));
            *wp = p + 14;
            move_block(e->model, e->stride, bpp, X + bmx, Y + bmy, X, Y, W, H);
        }
    }
    memset(e->lossy, 0, (size_t)e->tw * e->th);
    (*nrects)++;
    if (st) { st->copies++; st->rects++; }
    return 1;
}

/* ---------- tile diff + rect building ---------- */

/* hash of the (possibly clipped) tile at tx,ty in fb */
static uint64_t tile_hash(const g4_encoder *e, const uint8_t *fb, int tx, int ty)
{
    int y0 = ty * TILE, y1 = y0 + TILE > e->h ? e->h : y0 + TILE;
    int x0 = tx * TILE, x1 = x0 + TILE > e->w ? e->w : x0 + TILE;
    size_t off = (size_t)x0 * e->bpp, n = (size_t)(x1 - x0) * e->bpp;
    uint64_t h = 0xcbf29ce484222325ull;
    int y;
    for (y = y0; y < y1; y++) { h ^= hash_seg(fb + (size_t)y * e->stride + off, n); h *= 0x100000001b3ull; }
    return h;
}

static int diff_tiles(g4_encoder *e)
{
    int tx, ty, y, changed = 0;
    for (ty = 0; ty < e->th; ty++) {
        int y0 = ty * TILE, y1 = y0 + TILE > e->h ? e->h : y0 + TILE;
        for (tx = 0; tx < e->tw; tx++) {
            int x0 = tx * TILE, x1 = x0 + TILE > e->w ? e->w : x0 + TILE;
            size_t off = (size_t)x0 * e->bpp, n = (size_t)(x1 - x0) * e->bpp;
            int i = ty * e->tw + tx;
            uint8_t ch = 0;
            e->refresh[i] = 0;
            if (!e->all_dirty && !e->dmask[i] && !e->lossy[i]) {   /* nothing ingested here */
                e->tiles[i] = 0;
                continue;
            }
            if (e->lossy[i]) {
                /* the client shows a 4:2:2 version of this tile: it only
                   counts as changed if the source changed, or once it has
                   been still long enough for a lossless refresh */
                if (tile_hash(e, e->cur, tx, ty) != e->lhash[i]) ch = 1;
                else if (!e->clock_set || (uint32_t)(e->now_ms - e->llast[i]) >= YUV_REFRESH_MS) {
                    ch = 1; e->refresh[i] = 1;
                }
                e->tiles[i] = ch;
                changed += ch;
                continue;
            }
            for (y = y0; y < y1; y++) {
                size_t o = (size_t)y * e->stride + off;
                if (memcmp(e->cur + o, e->model + o, n)) { ch = 1; break; }
            }
            e->tiles[ty * e->tw + tx] = ch;
            changed += ch;
        }
    }
    return changed;
}

/* how recently each changed tile changed before: video-rate tiles aren't
   cached (they'd churn the cache) and may be sent as 4:2:2 */
static void track_video(g4_encoder *e)
{
    int i, n = e->tw * e->th;
    for (i = 0; i < n; i++) {
        uint32_t dt;
        if (!e->tiles[i]) continue;
        if (e->refresh[i]) { e->tvol[i] = 0; e->tfast[i] = 0; continue; }   /* not a real change */
        dt = (uint32_t)(e->now_ms - e->tlast[i]);
        e->tvol[i] = e->clock_set && e->tlast[i] && dt < 100;
        e->tfast[i] = e->clock_set && e->tlast[i] && dt < YUV_FAST_MS;
        if (e->clock_set) e->tlast[i] = e->now_ms ? e->now_ms : 1;
    }
}

static int build_rects(g4_encoder *e, const uint8_t *mask)
{
    int n = 0, ty, i;
    for (ty = 0; ty < e->th; ty++) {
        const uint8_t *row = mask + ty * e->tw;
        int y0 = ty * TILE, rh = (y0 + TILE > e->h ? e->h : y0 + TILE) - y0;
        int c = 0;
        /* rects still open after the previous row become merge candidates */
        for (i = 0; i < n; i++) if (e->rects[i].open == 1) e->rects[i].open = 2;

        while (c < e->tw) {
            int c0, c1, matched = 0;
            if (!row[c]) { c++; continue; }
            c0 = c;
            while (c < e->tw && row[c]) c++;
            c1 = c;
            for (i = 0; i < n; i++) {
                mrect *r = &e->rects[i];
                if (r->open == 2 && r->c0 == c0 && r->c1 == c1) {
                    r->h += rh; r->open = 1; matched = 1; break;
                }
            }
            if (!matched) {
                mrect *r = &e->rects[n++];
                r->c0 = c0; r->c1 = c1; r->open = 1;
                r->x = c0 * TILE; r->y = y0;
                r->w = (c1 * TILE > e->w ? e->w : c1 * TILE) - r->x;
                r->h = rh;
            }
        }
        for (i = 0; i < n; i++) if (e->rects[i].open == 2) e->rects[i].open = 0;
    }
    return n;
}

/* ---------- tile cache ---------- */

#define MAX_STORES_PER_FRAME 1536

static size_t ctab_home(const g4_encoder *e, uint64_t h) { return (size_t)(h ^ (h >> 31)) & (e->ctab_cap - 1); }

static void ctab_insert_raw(g4_encoder *e, int s)
{
    size_t k = ctab_home(e, e->chash[s]);
    while (e->ctab[k] != -1) k = (k + 1) & (e->ctab_cap - 1);
    e->ctab[k] = s;
    e->ctab_used++;
}

static void ctab_insert(g4_encoder *e, int s)
{
    if (e->ctab_used + 1 > e->ctab_cap * 3 / 4) {
        /* stale entries (evicted slots) accumulate; rebuild from live slots */
        size_t i; int t;
        for (i = 0; i < e->ctab_cap; i++) e->ctab[i] = -1;
        e->ctab_used = 0;
        for (t = 0; t < e->slots; t++) if (e->cvalid[t] && t != s) ctab_insert_raw(e, t);
    }
    ctab_insert_raw(e, s);
}

static int cache_find(const g4_encoder *e, uint64_t h, const uint8_t *px)
{
    size_t k = ctab_home(e, h), tb = TILE_BYTES(e);
    while (e->ctab[k] != -1) {
        int s = e->ctab[k];
        if (e->cvalid[s] && e->chash[s] == h && memcmp(e->cpx + (size_t)s * tb, px, tb) == 0) return s;
        k = (k + 1) & (e->ctab_cap - 1);
    }
    return -1;
}

static int cache_victim(g4_encoder *e)
{
    for (;;) {                       /* CLOCK: recently hit tiles get a second chance */
        int s = e->chand;
        e->chand = (s + 1) % e->slots;
        if (!e->cvalid[s]) return s;
        if (e->cref[s]) { e->cref[s] = 0; continue; }
        return s;
    }
}

static void gather_tile(const g4_encoder *e, const uint8_t *fb, int tx, int ty, uint8_t *dst)
{
    size_t rowb = (size_t)TILE * e->bpp, xoff = (size_t)tx * TILE * e->bpp;
    int y;
    for (y = 0; y < TILE; y++)
        memcpy(dst + (size_t)y * rowb, fb + (size_t)(ty * TILE + y) * e->stride + xoff, rowb);
}

static int put_tile_rect(g4_encoder *e, uint8_t **wp, int kind, int tx, int ty, int slot)
{
    size_t pos = (size_t)(*wp - e->out);
    uint8_t *p;
    if (ensure(&e->out, &e->out_cap, pos + 12)) return -1;
    p = e->out + pos;
    p[0] = (uint8_t)kind; p[1] = 0;
    g4_put16(p + 2, (uint32_t)(tx * TILE)); g4_put16(p + 4, (uint32_t)(ty * TILE));
    g4_put16(p + 6, TILE); g4_put16(p + 8, TILE);
    g4_put16(p + 10, (uint32_t)slot);
    *wp = p + 12;
    return 0;
}

/* replace changed tiles the client already has cached with 12-byte references */
static int cache_pass(g4_encoder *e, uint8_t **wp, int *nrects, int *changed, g4_enc_stats *st)
{
    size_t tb = TILE_BYTES(e);
    int tx, ty;
    if (ensure(&e->gather, &e->gather_cap, tb)) return -1;
    for (ty = 0; ty < e->th; ty++)
        for (tx = 0; tx < e->tw; tx++) {
            int i = ty * e->tw + tx, s;
            uint8_t *g = e->gather;
            e->tstate[i] = 0;
            if (!e->tiles[i]) continue;
            if ((tx + 1) * TILE > e->w || (ty + 1) * TILE > e->h) continue;   /* edge tile */

            gather_tile(e, e->cur, tx, ty, g);
            if (memcmp(g, g + e->bpp, tb - e->bpp) == 0) { e->tstate[i] = 2; continue; }  /* solid: FILL is cheaper */
            e->thash[i] = hash_seg(g, tb);
            e->tstate[i] = 1;
            s = cache_find(e, e->thash[i], g);
            if (s < 0) continue;

            if (put_tile_rect(e, wp, G4_RECT_CACHE, tx, ty, s)) return -1;
            {
                int y;
                size_t rowb = (size_t)TILE * e->bpp, xoff = (size_t)tx * TILE * e->bpp;
                for (y = 0; y < TILE; y++)
                    memcpy(e->model + (size_t)(ty * TILE + y) * e->stride + xoff, g + (size_t)y * rowb, rowb);
            }
            e->cref[s] = 1;
            if (e->refresh[i] || e->lossy[i]) { e->lossy[i] = 0; if (st) st->refreshes++; }
            e->tiles[i] = 0;
            e->tstate[i] = 0;
            (*changed)--;
            (*nrects)++;
            if (st) { st->cache_hits++; st->rects++; }
        }
    return 0;
}

/* remember tiles sent this frame (client copies them from its framebuffer) */
static int store_pass(g4_encoder *e, uint8_t **wp, int *nrects, int changed_total, g4_enc_stats *st)
{
    size_t tb = TILE_BYTES(e);
    int i, n = e->tw * e->th, stores = 0;
    if (ensure(&e->gather, &e->gather_cap, tb)) return -1;
    for (i = 0; i < n && stores < MAX_STORES_PER_FRAME; i++) {
        int tx = i % e->tw, ty = i / e->tw, s;
        if (!e->tiles[i] || e->tstate[i] != 1) continue;
        /* skip video-rate tiles, unless the whole update is tiny (caret) */
        if (e->tvol[i] && changed_total > 8) continue;
        gather_tile(e, e->cur, tx, ty, e->gather);
        if (cache_find(e, e->thash[i], e->gather) >= 0) continue;   /* duplicate on screen */
        s = cache_victim(e);
        memcpy(e->cpx + (size_t)s * tb, e->gather, tb);
        e->chash[s] = e->thash[i];
        e->cvalid[s] = 1;
        e->cref[s] = 0;
        ctab_insert(e, s);
        if (put_tile_rect(e, wp, G4_RECT_STORE, tx, ty, s)) return -1;
        (*nrects)++;
        stores++;
        if (st) st->cache_stores++;
    }
    return 0;
}

/* at most 256 colours (16 at 16-bit, where 8-bit indices gain nothing)? */
static int area_few_colours(const g4_encoder *e, int x0, int y0, int w, int h)
{
    uint32_t keys[512];
    uint8_t used[512];
    int n = 0, limit = e->bpp == 2 ? 16 : 256, x, y;
    memset(used, 0, sizeof used);
    for (y = y0; y < y0 + h; y++) {
        const uint8_t *row = e->cur + (size_t)y * e->stride + (size_t)x0 * e->bpp;
        uint32_t last = 0;
        int have_last = 0;
        for (x = 0; x < w; x++) {
            uint32_t c = px_at(row + (size_t)x * e->bpp, e->bpp), k;
            if (have_last && c == last) continue;           /* runs of one colour are common */
            last = c; have_last = 1;
            k = (c * 2654435761u) >> 23;
            while (used[k] && keys[k] != c) k = (k + 1) & 511;
            if (!used[k]) {
                if (++n > limit) return 0;
                used[k] = 1; keys[k] = c;
            }
        }
    }
    return 1;
}

static int tile_few_colours(const g4_encoder *e, int t)
{
    int tx = t % e->tw, ty = t / e->tw, x = tx * TILE, y = ty * TILE;
    return area_few_colours(e, x, y, x + TILE > e->w ? e->w - x : TILE, y + TILE > e->h ? e->h - y : TILE);
}

/* Few colours (text, interface)? Build palette + packed indices of the w x h
   pixels at g into e->pbuf, LZ4 them into e->plz if that's smaller.
   Returns the payload size after the rect header (0 = more than 256
   colours or not worth it); fills bits, n, pal and *clen. */
static size_t build_palette(g4_encoder *e, const uint8_t *g, int w, int h,
                            int *bits_out, int *n_out, uint8_t *pal, size_t *clen_out)
{
    const int bpp = e->bpp;
    uint32_t keys[512];
    int16_t  idx[512];
    int n = 0, bits, x, y;
    size_t rowpb, total, i, clen;

    memset(idx, 0xFF, sizeof idx);
    memset(keys, 0, sizeof keys);
    /* pass 1: count colours (stop past 256) */
    for (i = 0; i < (size_t)w * h; i++) {
        uint32_t c = px_at(g + i * bpp, bpp), k = (c * 2654435761u) >> 23;
        while (idx[k] >= 0 && keys[k] != c) k = (k + 1) & 511;
        if (idx[k] < 0) {
            if (n == 256) return 0;
            keys[k] = c; idx[k] = (int16_t)n;
            memcpy(pal + (size_t)n * bpp, g + i * bpp, (size_t)bpp);
            n++;
        }
    }
    bits = n <= 2 ? 1 : n <= 4 ? 2 : n <= 16 ? 4 : 8;
    if (bits == 8 && bpp == 2) return 0;          /* no gain over plain 16-bit pixels */
    rowpb = ((size_t)w * bits + 7) / 8;
    total = rowpb * h;
    if (ensure(&e->pbuf, &e->pbuf_cap, total)) return 0;
    if (ensure(&e->plz, &e->plz_cap, g4_lz4_bound(total))) return 0;
    /* pass 2: indices, high bits first, each row starting on a byte */
    memset(e->pbuf, 0, total);
    for (y = 0; y < h; y++) {
        uint8_t *o = e->pbuf + (size_t)y * rowpb;
        const uint8_t *row = g + (size_t)y * w * bpp;
        for (x = 0; x < w; x++) {
            uint32_t c = px_at(row + (size_t)x * bpp, bpp), k = (c * 2654435761u) >> 23;
            unsigned v, bit;
            while (keys[k] != c) k = (k + 1) & 511;
            v = (unsigned)idx[k];
            bit = (unsigned)x * bits;
            o[bit >> 3] |= (uint8_t)(v << (8 - bits - (bit & 7)));
        }
    }
    clen = g4_lz4_compress(e->pbuf, total, e->plz, e->plz_cap);
    if (!(clen && clen < total)) clen = 0;
    *bits_out = bits; *n_out = n; *clen_out = clen;
    return 2 + (size_t)n * bpp + 4 + (clen ? clen : total);
}

static int encode_rect(g4_encoder *e, const mrect *r, uint8_t **wp, g4_enc_stats *st)
{
    size_t rowb = (size_t)r->w * e->bpp, raw = rowb * r->h, xoff = (size_t)r->x * e->bpp;
    size_t pos = (size_t)(*wp - e->out);
    uint8_t *g, *p;
    int y;

    if (ensure(&e->gather, &e->gather_cap, raw)) return -1;
    if (ensure(&e->lz, &e->lz_cap, g4_lz4_bound(raw))) return -1;
    g = e->gather;
    for (y = 0; y < r->h; y++) {
        size_t o = (size_t)(r->y + y) * e->stride + xoff;
        memcpy(g + (size_t)y * rowb, e->cur + o, rowb);
        memcpy(e->model + o, e->cur + o, rowb);     /* client will now hold this */
    }
    if (st) st->raw_bytes += (uint32_t)raw;
    {
        int tx, ty;
        for (ty = r->y / TILE; ty * TILE < r->y + r->h; ty++)
            for (tx = r->x / TILE; tx * TILE < r->x + r->w; tx++) {
                int i = ty * e->tw + tx;
                if (e->lossy[i]) { e->lossy[i] = 0; if (st) st->refreshes++; }
            }
    }

    if (ensure(&e->out, &e->out_cap, pos + 14 + raw)) return -1;
    p = e->out + pos;
    p[1] = 0;
    g4_put16(p + 2, (uint32_t)r->x); g4_put16(p + 4, (uint32_t)r->y);
    g4_put16(p + 6, (uint32_t)r->w); g4_put16(p + 8, (uint32_t)r->h);

    if (raw <= (size_t)e->bpp || memcmp(g, g + e->bpp, raw - e->bpp) == 0) {
        uint32_t col = e->bpp == 4 ? g4_get32(g) : g4_get16(g);
        p[0] = G4_RECT_FILL;
        g4_put32(p + 10, col);
        *wp = p + 14;
        if (st) st->fills++;
    } else {
        size_t clen = g4_lz4_compress(g, raw, e->lz, e->lz_cap), plen = 0, pclen = 0;
        int pbits = 0, pn = 0;
        uint8_t palette[256 * 4];
        if (e->pal) plen = build_palette(e, g, r->w, r->h, &pbits, &pn, palette, &pclen);
        if (plen && plen < (clen ? clen + 4 : raw)) {
            size_t total = ((size_t)r->w * pbits + 7) / 8 * r->h;
            if (ensure(&e->out, &e->out_cap, pos + 10 + plen)) return -1;
            p = e->out + pos;
            p[0] = G4_RECT_PAL;
            p[10] = (uint8_t)pbits; p[11] = (uint8_t)(pn - 1);
            memcpy(p + 12, palette, (size_t)pn * e->bpp);
            g4_put32(p + 12 + (size_t)pn * e->bpp, (uint32_t)pclen);
            memcpy(p + 16 + (size_t)pn * e->bpp, pclen ? e->plz : e->pbuf, pclen ? pclen : total);
            *wp = p + 10 + plen;
            if (st) st->pals++;
        } else if (clen && clen + 4 < raw) {
            if (ensure(&e->out, &e->out_cap, pos + 14 + clen)) return -1;
            p = e->out + pos;
            p[0] = G4_RECT_LZ4;
            g4_put32(p + 10, (uint32_t)clen);
            memcpy(p + 14, e->lz, clen);
            *wp = p + 14 + clen;
            if (st) st->lz4s++;
        } else {
            p[0] = G4_RECT_RAW;
            memcpy(p + 10, g, raw);
            *wp = p + 10 + raw;
            if (st) st->raws++;
        }
    }
    if (st) st->rects++;
    return 0;
}

/* one video rect as 4:2:2; the model gets exactly what the client will show */
static int encode_rect_yuv(g4_encoder *e, const mrect *r, uint8_t **wp, g4_enc_stats *st)
{
    int pairs = (r->w + 1) / 2, y, k;
    size_t rowy = (size_t)pairs * 4, raw = rowy * r->h, xoff = (size_t)r->x * 4;
    size_t pos = (size_t)(*wp - e->out), clen;
    uint8_t *p;

    if (ensure(&e->gather, &e->gather_cap, raw)) return -1;
    if (ensure(&e->lz, &e->lz_cap, g4_lz4_bound(raw))) return -1;
    for (y = 0; y < r->h; y++) {
        const uint8_t *c = e->cur + (size_t)(r->y + y) * e->stride + xoff;
        uint8_t *m = e->model + (size_t)(r->y + y) * e->stride + xoff;
        uint8_t *g = e->gather + (size_t)y * rowy;
        for (k = 0; k < pairs; k++) {
            int x0 = 2 * k, x1 = x0 + 1 < r->w ? x0 + 1 : x0;
            uint8_t tmp[4];
            g4_yuv_from_pair(c + 4 * x0, c + 4 * x1, g + 4 * k);
            if (x1 != x0) g4_yuv_to_pair(g + 4 * k, m + 4 * x0, m + 4 * x1);
            else          g4_yuv_to_pair(g + 4 * k, m + 4 * x0, tmp);
        }
    }
    if (st) { st->raw_bytes += (uint32_t)((size_t)r->w * r->h * 4); st->yuvs++; st->rects++; }

    clen = g4_lz4_compress(e->gather, raw, e->lz, e->lz_cap);
    if (!(clen && clen + 4 < raw)) clen = 0;
    if (ensure(&e->out, &e->out_cap, pos + 14 + (clen ? clen : raw))) return -1;
    p = e->out + pos;
    p[0] = G4_RECT_YUV; p[1] = 0;
    g4_put16(p + 2, (uint32_t)r->x); g4_put16(p + 4, (uint32_t)r->y);
    g4_put16(p + 6, (uint32_t)r->w); g4_put16(p + 8, (uint32_t)r->h);
    g4_put32(p + 10, (uint32_t)clen);
    memcpy(p + 14, clen ? e->lz : e->gather, clen ? clen : raw);
    *wp = p + 14 + (clen ? clen : raw);
    return 0;
}

/* one video rect at half resolution: each half-res pixel is the average of
   a 2x2 block; the model gets what the client paints (g4_yuvh_paint) */
static int encode_rect_yuvh(g4_encoder *e, const mrect *r, uint8_t **wp, g4_enc_stats *st)
{
    unsigned w = (unsigned)r->w, h = (unsigned)r->h, hw = (w + 1) / 2, hh = (h + 1) / 2;
    unsigned pairs = (hw + 1) / 2, j, k;
    size_t rowy = (size_t)pairs * 4, raw = rowy * hh, xoff = (size_t)r->x * 4;
    size_t pos = (size_t)(*wp - e->out), clen;
    uint8_t *p;

    if (ensure(&e->gather, &e->gather_cap, raw)) return -1;
    if (ensure(&e->lz, &e->lz_cap, g4_lz4_bound(raw))) return -1;
    if (ensure(&e->hrows, &e->hrows_cap, (size_t)hw * 8 + 8)) return -1;
    for (j = 0; j < hh; j++) {
        const uint8_t *r0 = e->cur + (size_t)(r->y + 2 * j) * e->stride + xoff;
        const uint8_t *r1 = 2 * j + 1 < h ? r0 + e->stride : r0;
        uint8_t *g = e->gather + (size_t)j * rowy;
        for (k = 0; k < pairs; k++) {
            uint32_t q[2];
            int t;
            for (t = 0; t < 2; t++) {
                unsigned hx = 2 * k + (unsigned)t < hw ? 2 * k + (unsigned)t : hw - 1;
                unsigned x0 = 2 * hx, x1 = x0 + 1 < w ? x0 + 1 : x0;
                q[t] = avg_px(avg_px(ld32(r0 + 4 * x0), ld32(r0 + 4 * x1)),
                              avg_px(ld32(r1 + 4 * x0), ld32(r1 + 4 * x1)));
            }
            g4_yuv_from_pair((const uint8_t *)&q[0], (const uint8_t *)&q[1], g + 4 * k);
        }
    }
    g4_yuvh_paint(e->gather, w, h, e->model + (size_t)r->y * e->stride + xoff, e->stride, e->hrows);
    if (st) { st->raw_bytes += (uint32_t)((size_t)w * h * 4); st->yuvs++; st->rects++; }

    clen = g4_lz4_compress(e->gather, raw, e->lz, e->lz_cap);
    if (!(clen && clen + 4 < raw)) clen = 0;
    if (ensure(&e->out, &e->out_cap, pos + 14 + (clen ? clen : raw))) return -1;
    p = e->out + pos;
    p[0] = G4_RECT_YUVH; p[1] = 0;
    g4_put16(p + 2, (uint32_t)r->x); g4_put16(p + 4, (uint32_t)r->y);
    g4_put16(p + 6, (uint32_t)w);    g4_put16(p + 8, (uint32_t)h);
    g4_put32(p + 10, (uint32_t)clen);
    memcpy(p + 14, clen ? e->lz : e->gather, clen ? clen : raw);
    *wp = p + 14 + (clen ? clen : raw);
    return 0;
}

/* send tiles changing at video rate as 4:2:2 */
static int yuv_pass(g4_encoder *e, uint8_t **wp, int *nrects, int *changed, g4_enc_stats *st)
{
    int i, n = e->tw * e->th, nv = 0, nr;
    if (!e->yuv || !e->clock_set) return 0;
    for (i = 0; i < n; i++) {
        e->vmask[i] = e->tiles[i] && e->tfast[i] && !e->refresh[i];
        nv += e->vmask[i];
    }
    if (nv < YUV_MIN_TILES) return 0;
    nr = build_rects(e, e->vmask);
    for (i = 0; i < nr; i++) {
        const mrect *r = &e->rects[i];
        int tx, ty;
        if (e->yuvh && r->w >= 16 && r->h >= 16 ? encode_rect_yuvh(e, r, wp, st) : encode_rect_yuv(e, r, wp, st)) return -1;
        for (ty = r->y / TILE; ty * TILE < r->y + r->h; ty++)
            for (tx = r->x / TILE; tx * TILE < r->x + r->w; tx++) {
                int t = ty * e->tw + tx;
                e->tiles[t] = 0;
                e->lossy[t] = 1;
                e->lhash[t] = tile_hash(e, e->cur, tx, ty);
                e->llast[t] = e->now_ms;
                (*changed)--;
            }
    }
    *nrects += nr;
    return 0;
}

size_t g4enc_encode(g4_encoder *e, uint32_t frame_id, const uint8_t **out, g4_enc_stats *st)
{
    uint8_t *wp;
    int nrects = 0, nr, i, changed;

    if (st) memset(st, 0, sizeof *st);
    if (ensure(&e->out, &e->out_cap, 64)) return 0;
    wp = e->out + G4_HDR_SIZE + 8;

    if (e->force_full) {
        memset(e->tiles, 1, (size_t)e->tw * e->th);
        memset(e->refresh, 0, (size_t)e->tw * e->th);
        changed = e->tw * e->th;
        e->force_full = 0;
    } else {
        int copied = 0;
        for (i = 0; i < e->nhints; i++) {
            int *h = e->hints[i];
            copied |= detect_scroll(e, h[0], h[1], h[2], h[3], &wp, &nrects, st);
        }
        changed = diff_tiles(e);
        if (!copied && changed >= 8) {
            /* no usable hint: try the bounding box of everything that changed */
            int tx, ty, x0 = e->tw, y0 = e->th, x1 = -1, y1 = -1;
            for (ty = 0; ty < e->th; ty++)
                for (tx = 0; tx < e->tw; tx++)
                    if (e->tiles[ty * e->tw + tx]) {
                        if (tx < x0) x0 = tx;
                        if (tx > x1) x1 = tx;
                        if (ty < y0) y0 = ty;
                        if (ty > y1) y1 = ty;
                    }
            {
                int bx = x0 * TILE, by = y0 * TILE;
                int bw = ((x1 + 1) * TILE > e->w ? e->w : (x1 + 1) * TILE) - bx;
                int bh = ((y1 + 1) * TILE > e->h ? e->h : (y1 + 1) * TILE) - by;
                if (detect_scroll(e, bx, by, bw, bh, &wp, &nrects, st) ||
                    detect_move(e, bx, by, bw, bh, &wp, &nrects, st))      /* dragged window, sideways scroll */
                    changed = diff_tiles(e);
            }
        }
    }
    e->nhints = 0;
    /* everything ingested so far has now been compared */
    memset(e->dmask, 0, (size_t)e->tw * e->th);
    e->all_dirty = 0;
    track_video(e);

    {
        int changed_total = changed;
        if (e->slots && cache_pass(e, &wp, &nrects, &changed, st)) return 0;
        if (yuv_pass(e, &wp, &nrects, &changed, st)) return 0;
        if (changed == 0 && nrects == 0) return 0;

        if (changed && e->pal) {
            /* few-colour tiles (text, interface) and the rest (photos,
               gradients) become separate rects, so one photo on screen
               doesn't stop the text around it from using a palette */
            int t, n = e->tw * e->th, pass;
            for (t = 0; t < n; t++) e->vmask[t] = e->tiles[t] && tile_few_colours(e, t);
            for (pass = 0; pass < 2; pass++) {
                if (pass) for (t = 0; t < n; t++) e->vmask[t] = e->tiles[t] && !e->vmask[t];
                nr = build_rects(e, e->vmask);
                for (i = 0; i < nr; i++) {
                    mrect r = e->rects[i];
                    if (pass == 0 && r.h > TILE && !area_few_colours(e, r.x, r.y, r.w, r.h)) {
                        /* together they may exceed 256 colours: one strip of tiles at a time */
                        int y;
                        for (y = r.y; y < r.y + r.h; y += TILE) {
                            mrect sr = r;
                            sr.y = y; sr.h = y + TILE > r.y + r.h ? r.y + r.h - y : TILE;
                            if (encode_rect(e, &sr, &wp, st)) return 0;
                            nrects++;
                        }
                    } else {
                        if (encode_rect(e, &r, &wp, st)) return 0;
                        nrects++;
                    }
                }
            }
        } else {
            nr = changed ? build_rects(e, e->tiles) : 0;
            for (i = 0; i < nr; i++)
                if (encode_rect(e, &e->rects[i], &wp, st)) return 0;
            nrects += nr;
        }

        /* STOREs go last: the client copies tiles after the frame is decoded */
        if (e->slots && changed && store_pass(e, &wp, &nrects, changed_total, st)) return 0;
    }

    {
        uint32_t len = (uint32_t)(wp - e->out);
        g4_put_header(e->out, G4_MSG_FRAME, len - G4_HDR_SIZE);
        g4_put32(e->out + G4_HDR_SIZE, frame_id);
        g4_put16(e->out + G4_HDR_SIZE + 4, (uint32_t)nrects);
        g4_put16(e->out + G4_HDR_SIZE + 6, 0);
        if (st) st->wire_bytes = len;
        *out = e->out;
        return len;
    }
}

/* ======================================================================
 * Decoder
 * ==================================================================== */

int g4dec_init(g4_decoder *d, uint8_t *fb, int width, int height, int depth)
{
    memset(d, 0, sizeof *d);
    if (depth != 16 && depth != 32) return -1;
    d->fb = fb; d->width = width; d->height = height;
    d->bpp = depth == 16 ? 2 : 4;
    d->stride = (size_t)width * d->bpp;
    d->nbands = (height + G4_DIRTY_BAND - 1) / G4_DIRTY_BAND;
    d->dirty_bands = (uint8_t *)calloc((size_t)d->nbands, 1);
    if (!d->dirty_bands) return -1;
    g4dec_reset_dirty(d);
    return 0;
}

int g4dec_enable_cache(g4_decoder *d, int slots)
{
    free(d->cache);
    d->cache = NULL; d->cache_slots = 0;
    if (slots <= 0) return 0;
    if (slots > G4_MAX_SLOTS) slots = G4_MAX_SLOTS;
    d->cache = (uint8_t *)malloc((size_t)slots * G4_TILE * G4_TILE * d->bpp);
    if (!d->cache) return -1;
    d->cache_slots = slots;
    return 0;
}

void g4dec_free(g4_decoder *d)
{
    free(d->scratch); d->scratch = NULL; d->scratch_cap = 0;
    free(d->cache); d->cache = NULL; d->cache_slots = 0;
    free(d->dirty_bands); d->dirty_bands = NULL; d->nbands = 0;
}
void g4dec_reset_dirty(g4_decoder *d)
{
    d->dirty_y0 = d->height; d->dirty_y1 = 0;
    if (d->dirty_bands) memset(d->dirty_bands, 0, (size_t)d->nbands);
}

static void mark_dirty(g4_decoder *d, int y, int h)
{
    int b;
    if (y < d->dirty_y0) d->dirty_y0 = y;
    if (y + h > d->dirty_y1) d->dirty_y1 = y + h;
    if (d->dirty_bands)
        for (b = y / G4_DIRTY_BAND; b * G4_DIRTY_BAND < y + h && b < d->nbands; b++) d->dirty_bands[b] = 1;
}

int g4dec_frame(g4_decoder *d, const uint8_t *p, size_t len, uint32_t *frame_id)
{
    const uint8_t *end = p + len;
    unsigned n, i;

    if (len < 8) return -1;
    if (frame_id) *frame_id = g4_get32(p);
    n = g4_get16(p + 4);
    p += 8;

    for (i = 0; i < n; i++) {
        unsigned kind, x, y, w, h;
        size_t rowb, xoff;
        int r;

        if (end - p < 10) return -2;
        kind = p[0];
        x = g4_get16(p + 2); y = g4_get16(p + 4);
        w = g4_get16(p + 6); h = g4_get16(p + 8);
        p += 10;
        if (w == 0 || h == 0 || x + w > (unsigned)d->width || y + h > (unsigned)d->height) return -3;
        rowb = (size_t)w * d->bpp;
        xoff = (size_t)x * d->bpp;

        switch (kind) {
        case G4_RECT_FILL: {
            uint32_t col;
            uint8_t *row0 = d->fb + (size_t)y * d->stride + xoff;
            unsigned k;
            if (end - p < 4) return -2;
            col = g4_get32(p); p += 4;
            if (d->bpp == 4) for (k = 0; k < w; k++) g4_put32(row0 + 4 * k, col);
            else             for (k = 0; k < w; k++) g4_put16(row0 + 2 * k, col);
            for (r = 1; r < (int)h; r++) memcpy(row0 + (size_t)r * d->stride, row0, rowb);
            break;
        }
        case G4_RECT_RAW: {
            size_t raw = rowb * h;
            if ((size_t)(end - p) < raw) return -2;
            for (r = 0; r < (int)h; r++)
                memcpy(d->fb + (size_t)(y + r) * d->stride + xoff, p + (size_t)r * rowb, rowb);
            p += raw;
            break;
        }
        case G4_RECT_LZ4: {
            size_t raw = rowb * h, clen;
            if (end - p < 4) return -2;
            clen = g4_get32(p); p += 4;
            if ((size_t)(end - p) < clen) return -2;
            if (x == 0 && w == (unsigned)d->width) {
                /* full-width band: contiguous in the framebuffer, decode in place */
                if (g4_lz4_decompress(p, clen, d->fb + (size_t)y * d->stride, raw)) return -4;
            } else {
                if (raw > d->scratch_cap) {
                    uint8_t *nb = (uint8_t *)realloc(d->scratch, raw);
                    if (!nb) return -5;
                    d->scratch = nb; d->scratch_cap = raw;
                }
                if (g4_lz4_decompress(p, clen, d->scratch, raw)) return -4;
                for (r = 0; r < (int)h; r++)
                    memcpy(d->fb + (size_t)(y + r) * d->stride + xoff, d->scratch + (size_t)r * rowb, rowb);
            }
            p += clen;
            break;
        }
        case G4_RECT_COPY: {
            unsigned sx, sy;
            if (end - p < 4) return -2;
            sx = g4_get16(p); sy = g4_get16(p + 2); p += 4;
            if (sx + w > (unsigned)d->width || sy + h > (unsigned)d->height) return -3;
            if (sx != x) {
                /* general move (not produced by the current encoder, kept for safety) */
                size_t sxoff = (size_t)sx * d->bpp;
                if (y <= sy) for (r = 0; r < (int)h; r++)
                    memmove(d->fb + (size_t)(y + r) * d->stride + xoff, d->fb + (size_t)(sy + r) * d->stride + sxoff, rowb);
                else for (r = (int)h - 1; r >= 0; r--)
                    memmove(d->fb + (size_t)(y + r) * d->stride + xoff, d->fb + (size_t)(sy + r) * d->stride + sxoff, rowb);
            } else {
                move_rows(d->fb, d->stride, xoff, rowb, (int)sy, (int)y, (int)h);
            }
            break;
        }
        case G4_RECT_CACHE:
        case G4_RECT_STORE: {
            unsigned slot;
            uint8_t *tile;
            if (end - p < 2) return -2;
            slot = g4_get16(p); p += 2;
            if (w != G4_TILE || h != G4_TILE || slot >= (unsigned)d->cache_slots) return -7;
            tile = d->cache + (size_t)slot * G4_TILE * G4_TILE * d->bpp;
            if (kind == G4_RECT_CACHE) {
                for (r = 0; r < G4_TILE; r++)
                    memcpy(d->fb + (size_t)(y + r) * d->stride + xoff, tile + (size_t)r * rowb, rowb);
            } else {
                for (r = 0; r < G4_TILE; r++)
                    memcpy(tile + (size_t)r * rowb, d->fb + (size_t)(y + r) * d->stride + xoff, rowb);
                continue;   /* screen unchanged: not dirty */
            }
            break;
        }
        case G4_RECT_YUV: {
            unsigned pairs = (w + 1) / 2;
            size_t rowy = (size_t)pairs * 4, raw = rowy * h, clen;
            const uint8_t *src;
            if (d->bpp != 4) return -8;
            if (end - p < 4) return -2;
            clen = g4_get32(p); p += 4;
            if (clen) {
                if ((size_t)(end - p) < clen) return -2;
                if (raw > d->scratch_cap) {
                    uint8_t *nb = (uint8_t *)realloc(d->scratch, raw);
                    if (!nb) return -5;
                    d->scratch = nb; d->scratch_cap = raw;
                }
                if (g4_lz4_decompress(p, clen, d->scratch, raw)) return -4;
                src = d->scratch;
                p += clen;
            } else {
                if ((size_t)(end - p) < raw) return -2;
                src = p;
                p += raw;
            }
            for (r = 0; r < (int)h; r++)
                g4_yuv_row(src + (size_t)r * rowy, d->fb + (size_t)(y + r) * d->stride + xoff, w);
            break;
        }
        case G4_RECT_PAL: {
            unsigned bits, n, k;
            size_t rowpb, total, clen;
            const uint8_t *src, *palp;
            uint8_t pal[256 * 4];
            if (end - p < 2) return -2;
            bits = p[0]; n = (unsigned)p[1] + 1; p += 2;
            if ((bits != 1 && bits != 2 && bits != 4 && bits != 8) || n > (1u << bits)) return -9;
            if ((size_t)(end - p) < (size_t)n * d->bpp + 4) return -2;
            palp = p; p += (size_t)n * d->bpp;
            memset(pal, 0, sizeof pal);
            memcpy(pal, palp, (size_t)n * d->bpp);
            for (k = n; k < 256; k++) memcpy(pal + k * d->bpp, palp, (size_t)d->bpp);   /* bad index: colour 0 */
            clen = g4_get32(p); p += 4;
            rowpb = ((size_t)w * bits + 7) / 8;
            total = rowpb * h;
            if (clen) {
                if ((size_t)(end - p) < clen) return -2;
                if (total > d->scratch_cap) {
                    uint8_t *nb = (uint8_t *)realloc(d->scratch, total);
                    if (!nb) return -5;
                    d->scratch = nb; d->scratch_cap = total;
                }
                if (g4_lz4_decompress(p, clen, d->scratch, total)) return -4;
                src = d->scratch;
                p += clen;
            } else {
                if ((size_t)(end - p) < total) return -2;
                src = p;
                p += total;
            }
            for (r = 0; r < (int)h; r++) {
                const uint8_t *in = src + (size_t)r * rowpb;
                uint8_t *o = d->fb + (size_t)(y + r) * d->stride + xoff;
                unsigned xx;
                if (bits == 8) {
                    if (d->bpp == 4) for (xx = 0; xx < w; xx++) memcpy(o + 4 * xx, pal + 4 * in[xx], 4);
                    else             for (xx = 0; xx < w; xx++) memcpy(o + 2 * xx, pal + 2 * in[xx], 2);
                } else {
                    unsigned mask = (1u << bits) - 1;
                    for (xx = 0; xx < w; ) {
                        unsigned byte = *in++;
                        int sh;
                        for (sh = 8 - (int)bits; sh >= 0 && xx < w; sh -= (int)bits, xx++) {
                            unsigned v = (byte >> sh) & mask;
                            if (d->bpp == 4) memcpy(o + 4 * xx, pal + 4 * v, 4);
                            else             memcpy(o + 2 * xx, pal + 2 * v, 2);
                        }
                    }
                }
            }
            break;
        }
        case G4_RECT_YUVH: {
            unsigned hw = (w + 1) / 2, hh = (h + 1) / 2, pairs = (hw + 1) / 2;
            size_t rowy = (size_t)pairs * 4, raw = rowy * hh, clen, need;
            const uint8_t *src;
            if (d->bpp != 4) return -8;
            if (end - p < 4) return -2;
            clen = g4_get32(p); p += 4;
            need = (clen ? raw : 0) + (size_t)hw * 8 + 8;      /* decompressed data + two rows */
            if (need > d->scratch_cap) {
                uint8_t *nb = (uint8_t *)realloc(d->scratch, need);
                if (!nb) return -5;
                d->scratch = nb; d->scratch_cap = need;
            }
            if (clen) {
                if ((size_t)(end - p) < clen) return -2;
                if (g4_lz4_decompress(p, clen, d->scratch, raw)) return -4;
                src = d->scratch;
                p += clen;
            } else {
                if ((size_t)(end - p) < raw) return -2;
                src = p;
                p += raw;
            }
            g4_yuvh_paint(src, w, h, d->fb + (size_t)y * d->stride + xoff, d->stride,
                          d->scratch + (clen ? raw : 0));
            break;
        }
        default:
            return -6;
        }
        mark_dirty(d, (int)y, (int)h);
    }
    return 0;
}
