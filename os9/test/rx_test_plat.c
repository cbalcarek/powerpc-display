/*
 * rx_test_plat.c — the receiver core on Linux, for testing against the fake
 * Macs in test/sim: a TCP listener on 5960, a framebuffer in memory, the same
 * scripted keyboard and mouse as test/sim/fake_tiger.c, and sound that's
 * only counted. One session, then it exits.
 *   W, H, DEPTH: the "screen" (default 1024 x 768, 16-bit)
 *   PAIR=y: answer the pairing question yes; NOKEYS=1: no scripted input
 */
#include "../rx_core.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

static int fd = -1;
static uint8_t *screen;
static int SW = 1024, SH = 768, SD = 16;
static double t0;
static int asked = 0;
static long sound_frames = 0, sound_nonzero = 0;

double plat_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

int plat_send(const uint8_t *p, size_t n)
{
    while (n) {
        ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EAGAIN || errno == EINTR) { usleep(1000); continue; }
            return -1;
        }
        p += w; n -= (size_t)w;
    }
    return 0;
}

int plat_recv(uint8_t *p, size_t cap)
{
    uint8_t c;
    ssize_t r = cap ? recv(fd, p, cap, 0) : recv(fd, &c, 1, MSG_PEEK);
    if (r > 0) return cap ? (int)r : 0;
    if (r == 0) return -1;
    return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? 0 : -1;
}

int plat_screen_info(int *w, int *h, int *d, int *mm) { *w = SW; *h = SH; *d = SD; *mm = 0; return 0; }
int plat_screen_take(rx_screen *s)
{
    if (!screen) screen = calloc((size_t)SW * SH, (size_t)SD / 8);
    s->base = screen; s->rowbytes = (long)SW * SD / 8; s->width = SW; s->height = SH; s->depth = SD;
    fprintf(stderr, "FAKE: took the screen %dx%d %d-bit\n", SW, SH, SD);
    return 0;
}
void plat_screen_release(void) { fprintf(stderr, "FAKE: released the screen\n"); }

void plat_input(int *x, int *y, int *buttons, unsigned char keys[16])
{
    double t = plat_ms() - t0;
    memset(keys, 0, 16);
    *x = 100; *y = 100; *buttons = 0;
    if (getenv("NOKEYS")) return;
#define SET(c) keys[(c) >> 3] |= (unsigned char)(1 << ((c) & 7))
    if (t > 1000 && t < 1150) { SET(55); SET(0); }
    if (t > 1300 && t < 1400) { SET(11); }
    if (t > 2200 && t < 2250) { SET(55); SET(53); }        /* Command-Escape */
    if (t > 1500 && t < 1700) { *x = 100 + (int)((t - 1500) / 2); *y = 120; }
    else if (t >= 1700) { *x = 200; *y = 120; }
    if (t > 1800 && t < 1900) *buttons = 1;
}

int  plat_sound_available(void) { return 1; }
void plat_sound(const int16_t *pcm, unsigned frames, unsigned ch, unsigned rate)
{
    unsigned i;
    (void)rate;
    sound_frames += frames;
    for (i = 0; i < frames * ch; i++) if (pcm[i]) sound_nonzero++;
}
void plat_sound_stop(void) {}

static const char *prefs_path(void)
{
    static char p[1024];
    snprintf(p, sizeof p, "%s/Library/Application Support/PowerPC Display/pairing", getenv("HOME"));
    return p;
}
size_t plat_prefs_load(char *buf, size_t cap)
{
    FILE *f = fopen(prefs_path(), "r");
    size_t n = 0;
    if (f) { n = fread(buf, 1, cap, f); fclose(f); }
    return n;
}
void plat_prefs_save(const char *buf, size_t n)
{
    FILE *f = fopen(prefs_path(), "w");
    if (f) { fwrite(buf, 1, n, f); fclose(f); }
}

void plat_status(int state, const char *text) { fprintf(stderr, "STATUS %d: %s\n", state, text); }
void plat_pair_prompt(const char *text) { fprintf(stderr, "PAIR? %s\n", text); asked = 1; }
void plat_log(const char *text) { fprintf(stderr, "%s\n", text); }
int  plat_cache_slots(int bpp) { (void)bpp; return 2048; }

int main(void)
{
    int lfd = socket(AF_INET, SOCK_STREAM, 0), one = 1, rc;
    struct sockaddr_in a;
    rx *r;
    if (getenv("W")) SW = atoi(getenv("W"));
    if (getenv("H")) SH = atoi(getenv("H"));
    if (getenv("DEPTH")) SD = atoi(getenv("DEPTH"));
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons(G4_PORT); a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(lfd, (struct sockaddr *)&a, sizeof a) || listen(lfd, 1)) { perror("listen"); return 1; }
    rx_init();
    fd = accept(lfd, NULL, NULL);
    if (fd < 0) return 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    fcntl(fd, F_SETFL, O_NONBLOCK);
    t0 = plat_ms();
    r = rx_begin();
    if (!r) return 1;
    while ((rc = rx_poll(r)) == RX_GOING) {
        if (asked) { asked = 0; rx_pair_answer(r, getenv("PAIR") && getenv("PAIR")[0] == 'y'); }
        usleep(rx_showing(r) ? 500 : 2000);
    }
    fprintf(stderr, "session ended (%d)\n", rc);
    rx_end(r);
    fprintf(stderr, "FAKE: played %ld samples, %ld non-silent\n", sound_frames * 2, sound_nonzero);
    close(fd);
    return 0;
}
