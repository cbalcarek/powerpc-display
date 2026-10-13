/* g4client.h — interface between the PowerPC Display engine (client.c) and
   the Tiger app wrapper (app.m). The command-line build doesn't use this. */
#ifndef G4CLIENT_H
#define G4CLIENT_H

enum {
    G4C_WAITING   = 0,   /* listening, advertised on Bonjour   */
    G4C_CONNECTED = 1,   /* showing the M1's screen            */
    G4C_PAUSED    = 2,   /* not listening (user paused)        */
    G4C_ERROR     = 3,
    G4C_BONJOUR_STUCK = 4  /* listening, but Bonjour isn't answering for us */
};

typedef void (*g4c_status_fn)(int state, const char *text);

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

#endif
