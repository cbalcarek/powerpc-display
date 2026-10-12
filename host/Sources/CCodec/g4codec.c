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

/* ======================================================================
 * Encoder
 * ==================================================================== */

#define TILE        G4_TILE
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

    uint64_t *hc, *hm;     /* row-segment hashes (current / model) */
    uint8_t  *uc, *um;     /* row segment is a single colour       */
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
    uint8_t  *tfast;       /* per tile: changed again within YUV_FAST_MS */
    uint8_t  *lossy;
    uint64_t *lhash;
    uint32_t *llast;
    uint8_t  *refresh;     /* per tile: changed only because it's due a lossless refresh */
    uint8_t  *vmask;
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
    e->hc = (uint64_t *)calloc(height, sizeof(uint64_t));
    e->hm = (uint64_t *)calloc(height, sizeof(uint64_t));
    e->uc = (uint8_t *)calloc(height, 1);
    e->um = (uint8_t *)calloc(height, 1);
    e->votes = (int32_t *)calloc((size_t)2 * height + 1, sizeof(int32_t));
    e->tcap = 1; while (e->tcap < (size_t)height * 2) e->tcap <<= 1;
    e->tkey = (uint64_t *)calloc(e->tcap, sizeof(uint64_t));
    e->tval = (int32_t *)calloc(e->tcap, sizeof(int32_t));
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
    }

    if (!e->cur || !e->model || !e->tiles || !e->rects || !e->hc || !e->hm ||
        !e->uc || !e->um || !e->votes || !e->tkey || !e->tval || !e->tlast || !e->tvol ||
        !e->tfast || !e->lossy || !e->lhash || !e->llast || !e->refresh || !e->vmask) {
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
    free(e->tkey); free(e->tval);
    free(e->cpx); free(e->chash); free(e->cvalid); free(e->cref); free(e->ctab);
    free(e->thash); free(e->tstate); free(e->tlast); free(e->tvol);
    free(e->tfast); free(e->lossy); free(e->lhash); free(e->llast); free(e->refresh); free(e->vmask);
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

void g4enc_ingest_bgra(g4_encoder *e, const uint8_t *src, size_t sstride)
{
    int y, x;
    for (y = 0; y < e->h; y++) {
        const uint8_t *s = src + (size_t)y * sstride;
        uint8_t *d = e->cur + (size_t)y * e->stride;
        if (e->depth == 32) {
            g4_bgra_to_wire32(s, d, (size_t)e->w);
        } else {
            for (x = 0; x < e->w; x++, s += 4, d += 2) {
                uint32_t v = 0x8000u | ((uint32_t)(s[2] >> 3) << 10) |
                             ((uint32_t)(s[1] >> 3) << 5) | (uint32_t)(s[0] >> 3);
                d[0] = (uint8_t)(v >> 8); d[1] = (uint8_t)v;
            }
        }
    }
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
 */
static int detect_scroll(g4_encoder *e, int rx, int ry, int rw, int rh,
                         uint8_t **wp, int *nrects, g4_enc_stats *st)
{
    size_t xoff = (size_t)rx * e->bpp, seg = (size_t)rw * e->bpp;
    int y, nonuni = 0, best_dy = 0, best_votes = 0, run0 = -1, runlen = 0, i;
    size_t mask = e->tcap - 1;

    if (rw < 64 || rh < 48) return 0;

    for (y = ry; y < ry + rh; y++) {
        const uint8_t *c = e->cur + (size_t)y * e->stride + xoff;
        const uint8_t *m = e->model + (size_t)y * e->stride + xoff;
        e->hc[y] = hash_seg(c, seg);
        e->hm[y] = hash_seg(m, seg);
        e->uc[y] = memcmp(c, c + e->bpp, seg - e->bpp) == 0;
        e->um[y] = memcmp(m, m + e->bpp, seg - e->bpp) == 0;
    }

    /* table: model row hash -> first row with that hash (non-uniform rows only) */
    for (i = 0; i < (int)e->tcap; i++) e->tval[i] = -1;
    for (y = ry; y < ry + rh; y++) {
        size_t k;
        if (e->um[y]) continue;
        k = (size_t)(e->hm[y] ^ (e->hm[y] >> 29)) & mask;
        while (e->tval[k] >= 0 && e->tkey[k] != e->hm[y]) k = (k + 1) & mask;
        if (e->tval[k] < 0) { e->tkey[k] = e->hm[y]; e->tval[k] = y; }
    }

    memset(e->votes + (e->h - rh), 0, sizeof(int32_t) * (size_t)(2 * rh + 1));
    for (y = ry; y < ry + rh; y++) {
        size_t k;
        if (e->uc[y]) continue;
        nonuni++;
        k = (size_t)(e->hc[y] ^ (e->hc[y] >> 29)) & mask;
        while (e->tval[k] >= 0) {
            if (e->tkey[k] == e->hc[y]) { e->votes[e->h + (e->tval[k] - y)]++; break; }
            k = (k + 1) & mask;
        }
    }
    for (i = -rh + 1; i < rh; i++) {
        if (i == 0) continue;
        if (e->votes[e->h + i] > best_votes) { best_votes = e->votes[e->h + i]; best_dy = i; }
    }
    if (best_dy == 0 || best_votes < 4 || best_votes * 4 < nonuni ||
        best_votes <= e->votes[e->h]) return 0;

    /* longest contiguous run of rows that match at best_dy */
    {
        int cur0 = -1, curlen = 0;
        for (y = ry; y < ry + rh; y++) {
            int sy = y + best_dy;
            int ok = sy >= ry && sy < ry + rh && e->hc[y] == e->hm[sy];
            if (ok) {
                if (cur0 < 0) { cur0 = y; curlen = 0; }
                curlen++;
                if (curlen > runlen) { runlen = curlen; run0 = cur0; }
            } else cur0 = -1;
        }
    }
    if (runlen < 16) return 0;

    /* emit COPY, apply to model */
    {
        size_t pos = (size_t)(*wp - e->out);
        uint8_t *p;
        if (ensure(&e->out, &e->out_cap, pos + 14)) return 0;   /* may move e->out */
        p = e->out + pos;
        p[0] = G4_RECT_COPY; p[1] = 0;
        g4_put16(p + 2, (uint32_t)rx);  g4_put16(p + 4, (uint32_t)run0);
        g4_put16(p + 6, (uint32_t)rw);  g4_put16(p + 8, (uint32_t)runlen);
        g4_put16(p + 10, (uint32_t)rx); g4_put16(p + 12, (uint32_t)(run0 + best_dy));
        *wp = p + 14;
    }
    move_rows(e->model, e->stride, xoff, seg, run0 + best_dy, run0, runlen);
    memset(e->lossy, 0, (size_t)e->tw * e->th);   /* lossy pixels moved: compare exactly from now on */
    (*nrects)++;
    if (st) { st->copies++; st->rects++; st->scroll_dy = best_dy; }
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
        size_t clen = g4_lz4_compress(g, raw, e->lz, e->lz_cap);
        if (clen && clen + 4 < raw) {
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
        if (encode_rect_yuv(e, r, wp, st)) return -1;
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
                if (detect_scroll(e, bx, by, bw, bh, &wp, &nrects, st))
                    changed = diff_tiles(e);
            }
        }
    }
    e->nhints = 0;
    track_video(e);

    {
        int changed_total = changed;
        if (e->slots && cache_pass(e, &wp, &nrects, &changed, st)) return 0;
        if (yuv_pass(e, &wp, &nrects, &changed, st)) return 0;
        if (changed == 0 && nrects == 0) return 0;

        nr = changed ? build_rects(e, e->tiles) : 0;
        for (i = 0; i < nr; i++)
            if (encode_rect(e, &e->rects[i], &wp, st)) return 0;
        nrects += nr;

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
            unsigned pairs = (w + 1) / 2, k;
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
            for (r = 0; r < (int)h; r++) {
                uint8_t *row = d->fb + (size_t)(y + r) * d->stride + xoff;
                const uint8_t *in = src + (size_t)r * rowy;
                for (k = 0; k < pairs; k++) {
                    uint8_t tmp[4];
                    if (2 * k + 1 < w) g4_yuv_to_pair(in + 4 * k, row + 8 * k, row + 8 * k + 4);
                    else               g4_yuv_to_pair(in + 4 * k, row + 8 * k, tmp);
                }
            }
            break;
        }
        default:
            return -6;
        }
        mark_dirty(d, (int)y, (int)h);
    }
    return 0;
}
