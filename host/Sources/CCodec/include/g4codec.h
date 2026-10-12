/*
 * g4codec.h — wire protocol + codec shared by the M1 host and the iMac G4 client.
 *
 * Design rules (all chosen to keep the 800 MHz G4 doing as little as possible):
 *   - Every multi-byte header field is BIG-ENDIAN (the G4's native order).
 *   - Pixels travel in the exact layout Tiger's OpenGL fast path wants, so the
 *     G4 never converts a pixel:
 *       depth 32: bytes A,R,G,B  -> GL_BGRA + GL_UNSIGNED_INT_8_8_8_8_REV
 *       depth 16: u16 BE 1-5-5-5 -> GL_BGRA + GL_UNSIGNED_SHORT_1_5_5_5_REV
 *   - The host keeps an exact model of the client's framebuffer and only sends
 *     what differs from it, so skipped/coalesced frames cost nothing.
 *   - Compression is LZ4 (block format): expensive-ish to compress on the M1,
 *     very cheap to decompress on the G4.
 *
 * Plain C99, builds with gcc 4.0 on Mac OS X 10.4 and with clang on macOS 15+.
 */
#ifndef G4CODEC_H
#define G4CODEC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define G4_PORT            5960
#define G4_PROTO_VERSION   1
#define G4_HDR_SIZE        8
#define G4_MAX_PAYLOAD     (32u * 1024u * 1024u)

/* message types (header byte 0) */
enum {
    G4_MSG_HELLO        = 1,  /* client -> host */
    G4_MSG_FRAME        = 2,  /* host -> client */
    G4_MSG_CURSOR_POS   = 3,  /* host -> client */
    G4_MSG_CURSOR_SHAPE = 4,  /* host -> client */
    G4_MSG_ACK          = 5,  /* client -> host: frame presented */
    G4_MSG_PING         = 6,
    G4_MSG_PONG         = 7
};

/* rect kinds inside a FRAME */
enum {
    G4_RECT_FILL  = 1,  /* + u32 colour (depth16: low 16 bits)   */
    G4_RECT_RAW   = 2,  /* + w*h*bpp pixel bytes                  */
    G4_RECT_LZ4   = 3,  /* + u32 clen + clen bytes (LZ4 block)    */
    G4_RECT_COPY  = 4,  /* + u16 srcx, u16 srcy (dst = x,y)       */
    G4_RECT_CACHE = 5,  /* + u16 slot: paint cached tile at x,y   */
    G4_RECT_STORE = 6   /* + u16 slot: save fb tile at x,y        */
};

/* tile cache: 32x32 tiles the client keeps so content it has seen before
   (menus closing, switching apps back, caret blink) costs 12 bytes */
#define G4_TILE             32
#define G4_DEFAULT_SLOTS    2048
#define G4_MAX_SLOTS        8192

/*
 * Message header: u8 type, u8 0, u16 0, u32 payload length (BE).
 *
 * HELLO   : 'G','4','D','P', u16 version, u16 width, u16 height,
 *           u8 depth (16|32), u8 flags, u16 cache slots, u16 0  (16 bytes)
 * FRAME   : u32 frame_id, u16 nrects, u16 0, then rects:
 *           u8 kind, u8 0, u16 x, u16 y, u16 w, u16 h, <kind data>
 *           Rects are applied in order (COPYs always come first).
 * CURSOR_POS  : i16 x, i16 y, u8 visible, u8 0, u16 0      (8 bytes)
 * CURSOR_SHAPE: u16 w, u16 h, u16 hotx, u16 hoty, then w*h*4 bytes of
 *               premultiplied A,R,G,B
 * ACK     : u32 frame_id
 * PING/PONG: u32 token
 */

/* ---------- byte helpers (endian-independent) ---------- */
static inline void g4_put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static inline void g4_put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static inline uint32_t g4_get16(const uint8_t *p) { return ((uint32_t)p[0] << 8) | p[1]; }
static inline uint32_t g4_get32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static inline void g4_put_header(uint8_t *p, int type, uint32_t len) {
    p[0] = (uint8_t)type; p[1] = 0; p[2] = 0; p[3] = 0; g4_put32(p + 4, len);
}

/* ---------- LZ4 block format ---------- */
size_t g4_lz4_bound(size_t n);
/* returns compressed size, or 0 if it would not fit in cap */
size_t g4_lz4_compress(const uint8_t *src, size_t n, uint8_t *dst, size_t cap);
/* returns 0 on success (exactly dlen bytes produced), -1 on malformed input */
int    g4_lz4_decompress(const uint8_t *src, size_t slen, uint8_t *dst, size_t dlen);

/* ---------- encoder (host side) ---------- */
typedef struct g4_encoder g4_encoder;

typedef struct {
    uint32_t rects, fills, raws, lz4s, copies, cache_hits, cache_stores;
    uint32_t raw_bytes;      /* uncompressed size of changed pixels */
    uint32_t wire_bytes;     /* size of the FRAME message           */
    int32_t  scroll_dy;      /* last detected scroll (0 = none)     */
} g4_enc_stats;

g4_encoder *g4enc_create(int width, int height, int depth);
void        g4enc_destroy(g4_encoder *e);
/* client state unknown (new connection): next encode sends everything and
   the tile cache starts empty */
void        g4enc_force_full(g4_encoder *e);
/* wall-clock milliseconds before each encode; lets the cache skip tiles that
   change at video rate. Optional. */
void        g4enc_set_time(g4_encoder *e, uint32_t ms);
/* use a tile cache of `slots` tiles (as advertised by the client); 0 = off */
int         g4enc_enable_cache(g4_encoder *e, int slots);
/* convert a BGRA8888 frame (ScreenCaptureKit layout) into the current frame */
void        g4enc_ingest_bgra(g4_encoder *e, const uint8_t *src, size_t stride);
/* dirty-rect hint (pixels) used for scroll detection; accumulates until encode */
void        g4enc_add_hint(g4_encoder *e, int x, int y, int w, int h);
/*
 * Encode everything that differs between the current frame and the client
 * model. Returns the size of a complete FRAME message (header included) at
 * *out, or 0 if nothing changed. The model is updated as if the client had
 * applied the message. *out stays valid until the next call.
 */
size_t      g4enc_encode(g4_encoder *e, uint32_t frame_id, const uint8_t **out, g4_enc_stats *st);
const uint8_t *g4enc_current(const g4_encoder *e);   /* wire-format current frame */
const uint8_t *g4enc_model(const g4_encoder *e);     /* what the client should hold */

/* pixel helpers for cursor images etc. */
void g4_bgra_to_wire32(const uint8_t *src, uint8_t *dst, size_t npix);

/* ---------- decoder (G4 side) ---------- */
typedef struct {
    uint8_t *fb;            /* width*height*bpp, row-major, top row first */
    int      width, height, bpp;
    size_t   stride;
    uint8_t *scratch;
    size_t   scratch_cap;
    int      dirty_y0, dirty_y1;   /* rows touched since last reset; y1 exclusive */
    uint8_t *cache;                /* slots * 32*32*bpp */
    int      cache_slots;
} g4_decoder;

int  g4dec_init(g4_decoder *d, uint8_t *fb, int width, int height, int depth);
int  g4dec_enable_cache(g4_decoder *d, int slots);   /* 0 ok, -1 out of memory */
void g4dec_free(g4_decoder *d);
void g4dec_reset_dirty(g4_decoder *d);
/* apply a FRAME payload (header stripped). 0 = ok, <0 = malformed */
int  g4dec_frame(g4_decoder *d, const uint8_t *p, size_t len, uint32_t *frame_id);

#ifdef __cplusplus
}
#endif
#endif
