/*
 * rx_core.h — the PowerPC Display receiver, without the system around it:
 * handshake and pairing, pictures, the pointer, keyboard and mouse, sound.
 * Plain C, single-threaded, driven by rx_poll() from the platform's event
 * loop. Used by the Mac OS 9 app (mac9.c).
 */
#ifndef RX_CORE_H
#define RX_CORE_H

#include <stdint.h>
#include "rx_platform.h"
#include "g4codec.h"
#include "g4auth.h"

#define RX_VERSION "0.14"

enum { RX_STATE_WAITING = 0, RX_STATE_PAIRING, RX_STATE_CONNECTED, RX_STATE_ERROR };

/* why rx_poll ended a session */
enum { RX_GOING = 0, RX_ENDED = -1, RX_USER_LEFT = -2, RX_HOST_SLEEPS = -3 };

typedef struct rx rx;

/* once at startup: reads (or creates) this Mac's id and the paired Macs */
void rx_init(void);
/* a new connection: allocates a session and sends HELLO. NULL = no memory */
rx  *rx_begin(void);
/* call often (every pass of the event loop). Returns RX_GOING, or why the
   session ended; then call rx_end. */
int  rx_poll(rx *r);
void rx_end(rx *r);
/* the person answered the pairing question */
void rx_pair_answer(rx *r, int yes);
/* the session has taken over the screen (poll as fast as possible then) */
int  rx_showing(const rx *r);
/* forget every paired Mac */
void rx_forget_pairings(void);
int  rx_paired_count(void);

#endif
