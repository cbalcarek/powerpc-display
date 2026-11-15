/*
 * rx_core.c — see rx_core.h. Speaks the same protocol as the Tiger app
 * (g4/client.c) and reuses the shared decoder and pairing code, so the Mac
 * app can't tell the two receivers apart.
 */
#include "rx_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* pairing: which Macs may use this display                            */

#define MAX_PAIRS 16
static uint8_t my_id[G4_ID_BYTES];
static int     my_id_ok = 0;
static struct { uint8_t host[G4_ID_BYTES]; uint8_t key[G4_KEY_BYTES]; } pairs[MAX_PAIRS];
static int     npairs = 0;

static void pairing_save(void)
{
    char buf[64 + MAX_PAIRS * 120], hh[2 * G4_ID_BYTES + 1], hk[2 * G4_KEY_BYTES + 1];
    size_t n = 0;
    int i;
    g4_hex(my_id, G4_ID_BYTES, hh);
    n += (size_t)sprintf(buf + n, "id %s\n", hh);
    for (i = 0; i < npairs; i++) {
        g4_hex(pairs[i].host, G4_ID_BYTES, hh);
        g4_hex(pairs[i].key, G4_KEY_BYTES, hk);
        n += (size_t)sprintf(buf + n, "host %s %s\n", hh, hk);
    }
    plat_prefs_save(buf, n);
}

static void pairing_load(void)
{
    char buf[64 + MAX_PAIRS * 120 + 1], *line, *next;
    size_t n = plat_prefs_load(buf, sizeof buf - 1);
    buf[n] = 0;
    npairs = 0;
    for (line = buf; line && *line; line = next) {
        char a[80], b[80];
        next = strchr(line, '\n');
        if (!next) next = strchr(line, '\r');
        if (next) *next++ = 0;
        if (sscanf(line, "id %64s", a) == 1 && strlen(a) == 2 * G4_ID_BYTES && !g4_unhex(a, my_id, G4_ID_BYTES))
            my_id_ok = 1;
        else if (npairs < MAX_PAIRS && sscanf(line, "host %64s %70s", a, b) == 2 &&
                 strlen(a) == 2 * G4_ID_BYTES && strlen(b) == 2 * G4_KEY_BYTES &&
                 !g4_unhex(a, pairs[npairs].host, G4_ID_BYTES) && !g4_unhex(b, pairs[npairs].key, G4_KEY_BYTES))
            npairs++;
    }
    if (!my_id_ok && !g4_random(my_id, G4_ID_BYTES)) { my_id_ok = 1; pairing_save(); }
}

static int pairing_find(const uint8_t *host)
{
    int i;
    for (i = 0; i < npairs; i++) if (!memcmp(pairs[i].host, host, G4_ID_BYTES)) return i;
    return -1;
}

static void pairing_store(const uint8_t *host, const uint8_t *key)
{
    int i = pairing_find(host);
    if (i < 0) {
        if (npairs == MAX_PAIRS) { memmove(&pairs[0], &pairs[1], sizeof pairs[0] * (MAX_PAIRS - 1)); npairs--; }
        i = npairs++;
    }
    memcpy(pairs[i].host, host, G4_ID_BYTES);
    memcpy(pairs[i].key, key, G4_KEY_BYTES);
    pairing_save();
}

void rx_init(void) { pairing_load(); }
void rx_forget_pairings(void) { pairing_load(); npairs = 0; pairing_save(); }
int  rx_paired_count(void) { return npairs; }

/* ------------------------------------------------------------------ */
/* the session                                                         */

enum { ST_AUTH, ST_PAIRING, ST_STREAMING };

struct rx {
    int      state;
    int      W, H, depth, bpp, wmm;
    rx_screen scr;
    int      shown;                     /* we have the screen */
    g4_decoder dec;
    uint8_t *fb;

    uint8_t *in;                        /* bytes received, not yet handled */
    size_t   in_off, in_len, in_cap;

    uint8_t  nonce[G4_NONCE_BYTES], cpub[G4_PUB_BYTES], cpriv[G4_PUB_BYTES];
    uint8_t  ikey[G4_TAG_BYTES];        /* signs our keyboard and mouse */
    uint8_t  pend_host[G4_ID_BYTES], pend_nh[G4_NONCE_BYTES], pend_key[G4_KEY_BYTES];
    int      pair_answer;               /* -1 waiting, 0 no, 1 yes */
    double   pair_deadline;

    /* the pointer: drawn into the picture */
    int      cur_x, cur_y, cur_vis, cur_w, cur_h, cur_hx, cur_hy;
    uint8_t *cur_px;                    /* premultiplied A,R,G,B */
    int      oc_x, oc_y, oc_w, oc_h;    /* where it was drawn last */
    int      cursor_dirty;

    /* keyboard and mouse */
    int      input_on, input_active;
    uint32_t input_seq;
    unsigned char prev_keys[16];
    int      px, py, pb;
    double   last_mouse_ms, local_move_ms;

    int      need_draw, got_frame, got_ping;
    uint32_t ack;
    double   last_rx, stat_t0, stat_decode, stat_draw;
    unsigned stat_frames, stat_bytes;
    int16_t  pcm[G4_AUDIO_MAX_FRAMES * 2];
};

int rx_showing(const rx *r) { return r && r->shown; }

static int send_msg(int type, const uint8_t *payload, uint32_t len)
{
    uint8_t m[G4_HDR_SIZE + 512];
    if (len > 512) return -1;
    g4_put_header(m, type, len);
    if (len) memcpy(m + G4_HDR_SIZE, payload, len);
    return plat_send(m, G4_HDR_SIZE + len);
}

static int send_u32(int type, uint32_t v)
{
    uint8_t b[4];
    g4_put32(b, v);
    return send_msg(type, b, 4);
}

rx *rx_begin(void)
{
    rx *r = (rx *)calloc(1, sizeof *r);
    uint8_t hello[G4_HDR_SIZE + G4_HELLO_V2_LEN];
    int slots;
    if (!r) return NULL;
    if (plat_screen_info(&r->W, &r->H, &r->depth, &r->wmm) || (r->depth != 16 && r->depth != 32)) {
        plat_status(RX_STATE_ERROR, "Set this Mac's screen to Thousands or Millions of colours (Monitors control panel).");
        free(r);
        return NULL;
    }
    r->bpp = r->depth / 8;
    r->pair_answer = -1;
    if (!my_id_ok) pairing_load();
    if (!my_id_ok || g4_random(r->nonce, sizeof r->nonce) || g4_dh_keypair(r->cpub, r->cpriv)) {
        plat_status(RX_STATE_ERROR, "Couldn't make random numbers; can't pair securely.");
        free(r);
        return NULL;
    }
    r->fb = (uint8_t *)calloc((size_t)r->W * r->H, (size_t)r->bpp);
    if (!r->fb || g4dec_init(&r->dec, r->fb, r->W, r->H, r->depth)) {
        plat_status(RX_STATE_ERROR, "Not enough memory for the picture. Give PowerPC Display more memory (Get Info).");
        free(r->fb); free(r);
        return NULL;
    }
    slots = plat_cache_slots(r->bpp);
    while (slots > 0 && g4dec_enable_cache(&r->dec, slots)) slots /= 2;   /* as many as fit */

    memset(hello, 0, sizeof hello);
    g4_put_header(hello, G4_MSG_HELLO, G4_HELLO_V2_LEN);
    memcpy(hello + G4_HDR_SIZE, "G4DP", 4);
    g4_put16(hello + G4_HDR_SIZE + 4, G4_PROTO_VERSION);
    g4_put16(hello + G4_HDR_SIZE + 6, (uint32_t)r->W);
    g4_put16(hello + G4_HDR_SIZE + 8, (uint32_t)r->H);
    hello[G4_HDR_SIZE + 10] = (uint8_t)r->depth;
    hello[G4_HDR_SIZE + 11] = (uint8_t)((r->depth == 32 ? G4_HELLO_FLAG_YUV | G4_HELLO_FLAG_YUVH : 0) |
                                        G4_HELLO_FLAG_AUTH | G4_HELLO_FLAG_PAL | G4_HELLO_FLAG_INPUT |
                                        (plat_sound_available() ? G4_HELLO_FLAG_AUDIO : 0));
    g4_put16(hello + G4_HDR_SIZE + 12, (uint32_t)r->dec.cache_slots);
    g4_put16(hello + G4_HDR_SIZE + 14, (uint32_t)(r->wmm > 0 && r->wmm < 65535 ? r->wmm : 0));
    memcpy(hello + G4_HDR_SIZE + 16, my_id, G4_ID_BYTES);
    memcpy(hello + G4_HDR_SIZE + 32, r->nonce, G4_NONCE_BYTES);
    memcpy(hello + G4_HDR_SIZE + 48, r->cpub, G4_PUB_BYTES);
    if (plat_send(hello, sizeof hello)) { rx_end(r); return NULL; }
    r->state = ST_AUTH;
    r->last_rx = r->stat_t0 = plat_ms();
    plat_status(RX_STATE_WAITING, "Your Mac is connecting...");
    return r;
}

void rx_end(rx *r)
{
    if (!r) return;
    if (r->shown) plat_screen_release();
    plat_sound_stop();
    memset(r->cpriv, 0, sizeof r->cpriv);
    memset(r->pend_key, 0, sizeof r->pend_key);
    memset(r->ikey, 0, sizeof r->ikey);
    g4dec_free(&r->dec);
    free(r->fb);
    free(r->in);
    free(r->cur_px);
    free(r);
}

/* ---- handshake ---- */

static int auth_fail(uint32_t why)
{
    uint8_t b[4];
    g4_put32(b, why);
    send_msg(G4_MSG_AUTH_FAIL, b, 4);
    return RX_ENDED;
}

static int auth_ok(rx *r, const uint8_t *key, const uint8_t *hid, const uint8_t *nh)
{
    uint8_t tag[G4_TAG_BYTES];
    g4_auth_tag(tag, key, 'C', my_id, hid, r->nonce, nh);
    g4_auth_tag(r->ikey, key, 'I', my_id, hid, r->nonce, nh);
    if (send_msg(G4_MSG_AUTH_OK, tag, G4_TAG_BYTES)) return RX_ENDED;
    r->state = ST_STREAMING;
    plat_status(RX_STATE_CONNECTED, "Connected. The picture appears when your Mac sends it. Command-Escape returns here.");
    return RX_GOING;
}

static int handle_auth(rx *r, const uint8_t *p, uint32_t len)
{
    const uint8_t *hid = p + 4, *nh = p + 20, *rest = p + 36;
    if (len < G4_AUTH_LEN) return RX_ENDED;
    if (p[0] == G4_AUTH_KNOWN) {
        uint8_t tag[G4_TAG_BYTES];
        int i = pairing_find(hid);
        if (i >= 0) g4_auth_tag(tag, pairs[i].key, 'H', my_id, hid, r->nonce, nh);
        if (i < 0 || !g4_equal(tag, rest, G4_TAG_BYTES)) {
            plat_status(RX_STATE_ERROR, "A Mac that isn't paired with this one tried to connect. It will ask to pair.");
            return auth_fail(G4_AUTH_UNKNOWN_HOST);
        }
        return auth_ok(r, pairs[i].key, hid, nh);
    }
    if (p[0] == G4_AUTH_PAIR) {
        char msg[300];
        unsigned code;
        if (g4_pair_key(r->pend_key, r->cpriv, rest, r->cpub, rest, my_id, hid)) return RX_ENDED;
        memcpy(r->pend_host, hid, G4_ID_BYTES);
        memcpy(r->pend_nh, nh, G4_NONCE_BYTES);
        code = g4_pair_code(r->pend_key);
        if (p[1] & G4_AUTH_FLAG_NOSCREEN)
            sprintf(msg, "A Mac with no screen of its own wants to use this Mac as its display. "
                         "Pair only if you're setting that Mac up to use this one.");
        else
            sprintf(msg, "Your Mac wants to use this Mac as a display. Pair only if your Mac shows the same code: %03u %03u",
                    code / 1000, code % 1000);
        r->state = ST_PAIRING;
        r->pair_answer = -1;
        r->pair_deadline = plat_ms() + 120000;
        plat_status(RX_STATE_PAIRING, msg);
        plat_pair_prompt(msg);
        return RX_GOING;
    }
    return RX_ENDED;
}

void rx_pair_answer(rx *r, int yes) { if (r && r->state == ST_PAIRING) r->pair_answer = yes ? 1 : 0; }

static int pairing_step(rx *r)
{
    int rc;
    if (r->pair_answer < 0) {
        if (plat_ms() > r->pair_deadline) r->pair_answer = 0;
        else return RX_GOING;
    }
    if (!r->pair_answer) {
        memset(r->pend_key, 0, sizeof r->pend_key);
        plat_status(RX_STATE_WAITING, "Not paired.");
        return auth_fail(G4_AUTH_DECLINED);
    }
    pairing_store(r->pend_host, r->pend_key);
    rc = auth_ok(r, r->pend_key, r->pend_host, r->pend_nh);
    memset(r->pend_key, 0, sizeof r->pend_key);
    plat_log("paired with a new Mac");
    return rc;
}

/* ---- the picture ---- */

static void set_cursor_shape(rx *r, const uint8_t *p, uint32_t len)
{
    unsigned w, h;
    uint8_t *np;
    if (len < 8) return;
    w = g4_get16(p); h = g4_get16(p + 2);
    if (w == 0 || h == 0 || w > 128 || h > 128 || len < 8 + (size_t)w * h * 4) return;
    np = (uint8_t *)realloc(r->cur_px, (size_t)w * h * 4);
    if (!np) return;
    memcpy(np, p + 8, (size_t)w * h * 4);
    r->cur_px = np;
    r->cur_w = (int)w; r->cur_h = (int)h;
    r->cur_hx = (int)g4_get16(p + 4); r->cur_hy = (int)g4_get16(p + 6);
    r->cursor_dirty = 1;
}

static void copy_rows(rx *r, int y0, int y1)
{
    size_t rowb = (size_t)r->W * r->bpp;
    int y;
    if (y0 < 0) y0 = 0;
    if (y1 > r->H) y1 = r->H;
    for (y = y0; y < y1; y++)
        memcpy(r->scr.base + (long)y * r->scr.rowbytes, r->fb + (size_t)y * r->dec.stride, rowb);
}

static void mark_rows(rx *r, int y, int h)
{
    int b;
    if (h <= 0) return;
    if (y < 0) { h += y; y = 0; }
    if (y + h > r->H) h = r->H - y;
    if (h <= 0) return;
    for (b = y / G4_DIRTY_BAND; b <= (y + h - 1) / G4_DIRTY_BAND && b < r->dec.nbands; b++) r->dec.dirty_bands[b] = 1;
    if (r->dec.dirty_y1 <= r->dec.dirty_y0) { r->dec.dirty_y0 = y; r->dec.dirty_y1 = y + h; }
    else {
        if (y < r->dec.dirty_y0) r->dec.dirty_y0 = y;
        if (y + h > r->dec.dirty_y1) r->dec.dirty_y1 = y + h;
    }
}

/* the pointer's spot: here while this mouse is moving it (no waiting for
   the other Mac's echo), else where the other Mac says */
static void cursor_spot(rx *r, int *x, int *y)
{
    if (r->input_active && plat_ms() - r->local_move_ms < 300) { *x = r->px; *y = r->py; }
    else { *x = r->cur_x; *y = r->cur_y; }
}

static void draw(rx *r)
{
    int cx, cy, sx0, sy0, b, start = -1;
    int show = r->cur_vis && r->cur_px && r->cur_w > 0;
    double t0 = plat_ms();

    cursor_spot(r, &cx, &cy);
    sx0 = cx - r->cur_hx; sy0 = cy - r->cur_hy;
    if (r->oc_w) mark_rows(r, r->oc_y, r->oc_h);            /* clean where it was */
    if (show) mark_rows(r, sy0, r->cur_h);
    for (b = 0; b <= r->dec.nbands; b++) {                    /* runs of touched 16-row bands */
        int on = b < r->dec.nbands && r->dec.dirty_bands[b];
        if (on && start < 0) start = b;
        if (!on && start >= 0) { copy_rows(r, start * G4_DIRTY_BAND, b * G4_DIRTY_BAND); start = -1; }
    }
    g4dec_reset_dirty(&r->dec);
    r->oc_w = 0;

    if (show) {
        int x, y;
        for (y = 0; y < r->cur_h; y++) {
            int sy = sy0 + y;
            if (sy < 0 || sy >= r->H) continue;
            for (x = 0; x < r->cur_w; x++) {
                int sx = sx0 + x;
                const uint8_t *s = r->cur_px + ((size_t)y * r->cur_w + x) * 4;
                unsigned a = s[0];
                if (sx < 0 || sx >= r->W || a == 0) continue;
                if (r->bpp == 4) {
                    uint8_t *d = r->scr.base + (long)sy * r->scr.rowbytes + (long)sx * 4;
                    d[1] = (uint8_t)(s[1] + d[1] * (255 - a) / 255);
                    d[2] = (uint8_t)(s[2] + d[2] * (255 - a) / 255);
                    d[3] = (uint8_t)(s[3] + d[3] * (255 - a) / 255);
                } else if (a >= 128) {
                    uint8_t *d = r->scr.base + (long)sy * r->scr.rowbytes + (long)sx * 2;
                    unsigned v = ((unsigned)(s[1] >> 3) << 10) | ((unsigned)(s[2] >> 3) << 5) | (s[3] >> 3);
                    d[0] = (uint8_t)(v >> 8); d[1] = (uint8_t)v;
                }
            }
        }
        r->oc_x = sx0; r->oc_y = sy0; r->oc_w = r->cur_w; r->oc_h = r->cur_h;
    }
    r->cursor_dirty = 0;
    r->stat_draw += plat_ms() - t0;
}

/* ---- keyboard and mouse ---- */

static int key_bit(const unsigned char *k, int code) { return (k[code >> 3] >> (code & 7)) & 1; }
static int is_modifier(int code) { return code >= 54 && code <= 63; }

static void put_event(uint8_t *e, int kind, int a, int b, int c)
{
    memset(e, 0, G4_INPUT_EVENT_LEN);
    e[0] = (uint8_t)kind; e[1] = (uint8_t)a;
    g4_put16(e + 2, (uint32_t)(uint16_t)(int16_t)b);
    g4_put16(e + 4, (uint32_t)(uint16_t)(int16_t)c);
}

static int send_input(rx *r, const uint8_t *ev, int n)
{
    uint8_t m[G4_HDR_SIZE + 4 + G4_INPUT_MAX_EVENTS * G4_INPUT_EVENT_LEN + G4_INPUT_MAC_LEN];
    uint32_t body = 4 + (uint32_t)n * G4_INPUT_EVENT_LEN;
    g4_put_header(m, G4_MSG_INPUT, body + G4_INPUT_MAC_LEN);
    g4_put32(m + G4_HDR_SIZE, ++r->input_seq);
    memcpy(m + G4_HDR_SIZE + 4, ev, (size_t)n * G4_INPUT_EVENT_LEN);
    g4_input_mac(m + G4_HDR_SIZE + body, r->ikey, m + G4_HDR_SIZE, body);
    return plat_send(m, G4_HDR_SIZE + body + G4_INPUT_MAC_LEN);
}

/* returns RX_USER_LEFT on Command-Escape, RX_ENDED if sending failed */
static int input_step(rx *r)
{
    unsigned char cur[16];
    uint8_t ev[G4_INPUT_MAX_EVENTS * G4_INPUT_EVENT_LEN];
    int x, y, b, n = 0, pass, k, cmd;

    plat_input(&x, &y, &b, cur);
    if (x < 0) x = 0; else if (x >= r->W) x = r->W - 1;
    if (y < 0) y = 0; else if (y >= r->H) y = r->H - 1;
    cmd = key_bit(cur, 55) || key_bit(cur, 54);
    if (cmd && key_bit(cur, 53)) return RX_USER_LEFT;     /* Command-Escape: back to this Mac */
    if (!r->input_on || !r->shown) { r->input_active = 0; return RX_GOING; }
    if (!r->input_active) {
        /* starting: keys already held stay here; the other Mac's pointer
           only moves once this mouse does */
        memcpy(r->prev_keys, cur, 16);
        r->px = x; r->py = y; r->pb = b;
        r->input_active = 1;
        return RX_GOING;
    }
    for (pass = 0; pass < 4; pass++) {                    /* modifiers down first, up last */
        for (k = 0; k < 128; k++) {
            int was = key_bit(r->prev_keys, k), now = key_bit(cur, k), mod = is_modifier(k);
            if (was == now) continue;
            if ((pass == 0 && !(now && mod)) || (pass == 1 && !(now && !mod)) ||
                (pass == 2 && !(!now && !mod)) || (pass == 3 && !(!now && mod))) continue;
            if (n == G4_INPUT_MAX_EVENTS) { if (send_input(r, ev, n)) return RX_ENDED; n = 0; }
            put_event(ev + n * G4_INPUT_EVENT_LEN, G4_IN_KEY, k, 0, 0);
            ev[n++ * G4_INPUT_EVENT_LEN + 2] = (uint8_t)now;
        }
    }
    memcpy(r->prev_keys, cur, 16);
    if (b != r->pb || ((x != r->px || y != r->py) && plat_ms() - r->last_mouse_ms >= 8)) {
        if (x != r->px || y != r->py) { r->local_move_ms = plat_ms(); r->cursor_dirty = 1; }
        if (n == G4_INPUT_MAX_EVENTS) { if (send_input(r, ev, n)) return RX_ENDED; n = 0; }
        put_event(ev + n++ * G4_INPUT_EVENT_LEN, G4_IN_MOUSE, b, x, y);
        r->px = x; r->py = y; r->pb = b;
        r->last_mouse_ms = plat_ms();
    }
    if (n && send_input(r, ev, n)) return RX_ENDED;
    return RX_GOING;
}

/* ---- messages ---- */

static int handle_message(rx *r, int type, const uint8_t *p, uint32_t len)
{
    if (r->state == ST_AUTH) {
        if (type != G4_MSG_AUTH) return RX_ENDED;        /* nothing else before AUTH */
        return handle_auth(r, p, len);
    }
    if (r->state == ST_PAIRING) return RX_GOING;         /* the host waits for our answer */
    switch (type) {
    case G4_MSG_FRAME: {
        uint32_t fid = 0;
        double t0 = plat_ms();
        if (g4dec_frame(&r->dec, p, len, &fid)) { plat_log("bad frame"); return RX_ENDED; }
        r->stat_decode += plat_ms() - t0;
        r->stat_frames++;
        r->got_frame = 1;
        r->ack = fid;
        r->need_draw = 1;
        break;
    }
    case G4_MSG_CURSOR_POS:
        if (len >= 5) {
            int x = (int16_t)g4_get16(p), y = (int16_t)g4_get16(p + 2), v = p[4];
            if (x != r->cur_x || y != r->cur_y || v != r->cur_vis) {
                r->cur_x = x; r->cur_y = y; r->cur_vis = v;
                r->cursor_dirty = 1;
            }
        }
        break;
    case G4_MSG_CURSOR_SHAPE:
        set_cursor_shape(r, p, len);
        break;
    case G4_MSG_INPUT_ON:
        if (len >= 1) {
            r->input_on = p[0] != 0;
            if (!r->input_on) r->input_active = 0;
            plat_log(r->input_on ? "your Mac uses this keyboard and mouse while it's shown"
                                 : "keyboard and mouse stay with this Mac");
        }
        break;
    case G4_MSG_AUDIO: {
        unsigned rate = 0, ch = 0;
        int frames = g4_audio_decode(p, len, r->pcm, G4_AUDIO_MAX_FRAMES, &rate, &ch);
        if (frames > 0 && rate >= 8000 && rate <= 96000) plat_sound(r->pcm, (unsigned)frames, ch, rate);
        break;
    }
    case G4_MSG_PING:
        r->got_ping = 1;
        if (len >= 4 && send_u32(G4_MSG_PONG, g4_get32(p))) return RX_ENDED;
        break;
    case G4_MSG_SLEEP:
        return RX_HOST_SLEEPS;
    default:
        break;   /* updates, brightness: not for this Mac */
    }
    return RX_GOING;
}

/* read what's waiting; handle whole messages. RX_GOING or why it ended */
static int read_messages(rx *r)
{
    double t0 = plat_ms();
    for (;;) {
        int got, rc;
        uint32_t len;
        /* room for at least 32 KB more */
        if (r->in_off > 0 && (r->in_off == r->in_len || r->in_cap - r->in_len < 32768)) {
            memmove(r->in, r->in + r->in_off, r->in_len - r->in_off);
            r->in_len -= r->in_off; r->in_off = 0;
        }
        if (r->in_cap - r->in_len < 32768) {
            size_t nc = r->in_cap ? r->in_cap * 2 : 131072;
            uint8_t *nb = (uint8_t *)realloc(r->in, nc);
            if (!nb) { plat_log("out of memory for messages"); return RX_ENDED; }
            r->in = nb; r->in_cap = nc;
        }
        got = plat_recv(r->in + r->in_len, r->in_cap - r->in_len);
        if (got < 0) return RX_ENDED;
        r->in_len += (size_t)got;
        if (got > 0) { r->last_rx = plat_ms(); r->stat_bytes += (unsigned)got; }

        /* whole messages */
        while (r->in_len - r->in_off >= G4_HDR_SIZE) {
            const uint8_t *h = r->in + r->in_off;
            len = g4_get32(h + 4);
            if (len > G4_MAX_PAYLOAD) { plat_log("oversized message"); return RX_ENDED; }
            if (r->in_len - r->in_off < G4_HDR_SIZE + len) {
                /* make sure a big message will fit once it's all here */
                if (G4_HDR_SIZE + len > r->in_cap - r->in_off) {
                    size_t need = r->in_len - r->in_off + 32768 + G4_HDR_SIZE + len;
                    memmove(r->in, r->in + r->in_off, r->in_len - r->in_off);
                    r->in_len -= r->in_off; r->in_off = 0;
                    if (need > r->in_cap) {
                        uint8_t *nb = (uint8_t *)realloc(r->in, need);
                        if (!nb) { plat_log("out of memory for a big message"); return RX_ENDED; }
                        r->in = nb; r->in_cap = need;
                    }
                }
                break;
            }
            rc = handle_message(r, h[0], h + G4_HDR_SIZE, len);
            r->in_off += G4_HDR_SIZE + len;
            if (rc != RX_GOING) return rc;
            if (r->state == ST_PAIRING) return RX_GOING;
        }
        /* nothing more right now, or long enough without drawing */
        if (got == 0 || plat_ms() - t0 > 15) break;
    }
    return RX_GOING;
}

int rx_poll(rx *r)
{
    int rc;
    double now;
    if (r->state == ST_PAIRING) {
        rc = pairing_step(r);
        if (rc != RX_GOING) return rc;
        if (r->state == ST_PAIRING) {
            uint8_t c;
            /* still waiting: notice if the other Mac gives up */
            if (plat_recv(&c, 0) < 0) { plat_status(RX_STATE_WAITING, "Pairing cancelled on your Mac."); return RX_ENDED; }
            return RX_GOING;
        }
    }
    rc = read_messages(r);
    if (rc != RX_GOING) return rc;
    now = plat_ms();
    if (r->state == ST_AUTH && now - r->last_rx > 15000) {
        plat_status(RX_STATE_ERROR, "Your Mac didn't finish connecting (an older version of PowerPC Display?).");
        return RX_ENDED;
    }
    if (r->got_ping && now - r->last_rx > 4000) {
        plat_status(RX_STATE_WAITING, "Your Mac stopped responding (cable unplugged, or it's asleep).");
        return RX_ENDED;
    }
    if (r->state != ST_STREAMING) return RX_GOING;

    rc = input_step(r);
    if (rc != RX_GOING) return rc;

    /* take over the screen once there's a picture, so a connection that
       fails early never blanks this Mac */
    if (r->need_draw && !r->shown && r->got_frame) {
        if (plat_screen_take(&r->scr) || r->scr.width != r->W || r->scr.height != r->H || r->scr.depth != r->depth) {
            plat_status(RX_STATE_ERROR, "Couldn't take over the screen (did its size or colours change?).");
            return RX_ENDED;
        }
        r->shown = 1;
        mark_rows(r, 0, r->H);                            /* paint everything once */
    }
    if (r->shown && (r->need_draw || r->cursor_dirty)) {
        draw(r);
        r->need_draw = 0;
        if (r->ack) {                                     /* the frame is on screen */
            if (send_u32(G4_MSG_ACK, r->ack)) return RX_ENDED;
            r->ack = 0;
        }
    }

    if (now - r->stat_t0 > 5000) {
        double secs = (now - r->stat_t0) / 1000.0;
        if (r->stat_frames) {
            char line[200];
            sprintf(line, "%.1f frames/s, %.0f KB/s in, decode %.1f ms/frame, draw %.1f ms/frame (Mac OS 9 app %s)",
                    r->stat_frames / secs, r->stat_bytes / 1024.0 / secs,
                    r->stat_decode / r->stat_frames, r->stat_draw / r->stat_frames, RX_VERSION);
            send_msg(G4_MSG_STATS, (const uint8_t *)line, (uint32_t)strlen(line));
            plat_log(line);
        }
        r->stat_t0 = now;
        r->stat_frames = r->stat_bytes = 0;
        r->stat_decode = r->stat_draw = 0;
    }
    return RX_GOING;
}
