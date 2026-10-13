/* g4client.h — interface between the PowerPC Display engine (client.c) and
   the Tiger app wrapper (app.m). The command-line build doesn't use this. */
#ifndef G4CLIENT_H
#define G4CLIENT_H

enum {
    G4C_WAITING   = 0,   /* listening, advertised on Bonjour   */
    G4C_CONNECTED = 1,   /* showing the M1's screen            */
    G4C_PAUSED    = 2,   /* not listening (user paused)        */
    G4C_ERROR     = 3,
    G4C_BONJOUR_STUCK = 4, /* listening, but Bonjour isn't answering for us */
    G4C_PAIRING   = 5      /* a new Mac wants to pair: show the code, ask */
};

typedef void (*g4c_status_fn)(int state, const char *text);
/* hardware cursor: called on the engine thread with the pointer image
   (premultiplied A,R,G,B bytes, w*h*4) and hot spot; argb == NULL means
   "back to the normal arrow". The app sets it as the system cursor. */
typedef void (*g4c_cursor_fn)(const unsigned char *argb, int w, int h, int hotx, int hoty);
void g4c_set_cursor_hook(g4c_cursor_fn fn);
void g4c_set_hardware_cursor(int on);         /* default on (app only) */

void g4c_set_status_hook(g4c_status_fn fn);   /* called from the server thread */
void g4c_set_depth(int depth);                /* 16, 32, or 0 = automatic; applies to the next connection */
int  g4c_auto_depth(void);                    /* what automatic would pick on this Mac right now */
const char *g4c_machine_summary(void);        /* e.g. "PowerPC G4 (7450), 1024 MB RAM, 32 MB video memory" */
void g4c_set_paused(int paused);
int  g4c_is_paused(void);
void g4c_set_app_mode(int on);                /* Cmd+Esc pauses instead of quitting */
int  g4c_server_run(void);                    /* blocks until g4c_stop() */
void g4c_stop(void);
int  g4c_bonjour_stuck(void);
void g4c_pair_answer(int yes);                /* answer to G4C_PAIRING */
void g4c_forget_pairings(void);               /* every Mac must pair again */
int  g4c_paired_count(void);

#endif
