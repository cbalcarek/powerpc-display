/*
 * rx_platform.h — what the receiver core (rx_core.c) needs from the system
 * it runs on. Implemented by mac9.c (Mac OS 9, MacTCP + Toolbox) and by
 * test/rx_test_plat.c (Linux sockets, for testing against the fake Macs).
 *
 * Everything is single-threaded: the platform's event loop calls rx_poll()
 * often, and these functions never block for long.
 */
#ifndef RX_PLATFORM_H
#define RX_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* milliseconds from any fixed starting point */
double plat_ms(void);

/* the connection to the other Mac */
int  plat_send(const uint8_t *p, size_t n);      /* 0 = all sent, -1 = gone */
int  plat_recv(uint8_t *p, size_t cap);          /* >0 bytes, 0 = nothing yet, -1 = closed
                                                    (cap 0: just checks whether it closed) */

/* the screen. info: what the session will use (depth 16 or 32, switching a
   256-colour screen to thousands if it can); take: hide the menu bar and the
   pointer and hand over the framebuffer (called when the first picture
   arrives); release: give everything back */
typedef struct {
    uint8_t *base;          /* top-left pixel of the main screen */
    long     rowbytes;
    int      width, height, depth;
} rx_screen;
int  plat_screen_info(int *width, int *height, int *depth, int *width_mm);  /* 0 ok */
int  plat_screen_take(rx_screen *s);             /* 0 ok */
void plat_screen_release(void);

/* keyboard and mouse: pointer in screen pixels, buttons (bit 0 = main),
   and the 128 key bits as GetKeys returns them (byte k>>3, bit k&7) */
void plat_input(int *x, int *y, int *buttons, unsigned char keys[16]);

/* sound: queue interleaved 16-bit samples; available = can play at all */
int  plat_sound_available(void);
void plat_sound(const int16_t *pcm, unsigned frames, unsigned channels, unsigned rate);
void plat_sound_stop(void);

/* the pairing file (small text) */
size_t plat_prefs_load(char *buf, size_t cap);
void   plat_prefs_save(const char *buf, size_t n);

/* what's going on, for the window (RX_STATE_*), and the pairing question:
   show it with Pair / Don't Pair; the answer comes back via rx_pair_answer */
void plat_status(int state, const char *text);
void plat_pair_prompt(const char *text);
void plat_log(const char *text);

/* tile cache slots it can afford (each 32x32 pixels at bpp bytes) */
int  plat_cache_slots(int bpp);

#ifdef __cplusplus
}
#endif
#endif
