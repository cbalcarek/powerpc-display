/*
 * mac9.c — PowerPC Display for Mac OS 9 (and 8.6): the receiver core
 * (rx_core.c) plus everything Mac OS: a small status window, MacTCP for the
 * connection (Open Transport runs MacTCP programs), a UDP answer so the Mac
 * app can find this Mac (no Bonjour on OS 9), drawing straight into the
 * screen, GetKeys/GetMouse for keyboard and mouse, the Sound Manager for
 * sound, and the pairing file in the Preferences folder.
 *
 * Built with Retro68 (PowerPC, classic Toolbox). Any PowerPC Mac on Mac OS
 * 8.6-9.2.2, at thousands or millions of colours.
 */
#include <Quickdraw.h>
#include <Windows.h>
#include <Menus.h>
#include <Fonts.h>
#include <TextEdit.h>
#include <Dialogs.h>
#include <Controls.h>
#include <Events.h>
#include <ToolUtils.h>
#include <Resources.h>
#include <Devices.h>
#include <Files.h>
#include <Folders.h>
#include <Sound.h>
#include <Timer.h>
#include <LowMem.h>
#include <Processes.h>
#include <AppleEvents.h>
#include <MacTCP.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rx_core.h"

#define APP_NAME "PowerPC Display"

/* ------------------------------------------------------------------ */
/* time and randomness                                                 */

double plat_ms(void)
{
    UnsignedWide us;
    Microseconds(&us);
    return ((double)us.hi * 4294967296.0 + (double)us.lo) / 1000.0;
}

/* Mac OS 9 has no system random source. We keep a pool, stirred with the
   microsecond timer (interrupt jitter), mouse moves, event and packet
   timing, and hash it with SHA-512 for every request. */
static uint8_t pool[64];
static uint32_t pool_count;

static void stir(const void *p, size_t n)
{
    uint8_t buf[64 + 64];
    memcpy(buf, pool, 64);
    if (n > 64) n = 64;
    memcpy(buf + 64, p, n);
    g4_sha512(pool, buf, 64 + n);
}

static void stir_now(void)
{
    struct { UnsignedWide us; UInt32 ticks; Point mouse; long freemem; uint32_t n; } e;
    Microseconds(&e.us);
    e.ticks = TickCount();
    GetMouse(&e.mouse);
    e.freemem = FreeMem();
    e.n = ++pool_count;
    stir(&e, sizeof e);
}

int g4_random(uint8_t *buf, size_t n)
{
    int i;
    for (i = 0; i < 512; i++) {                 /* timer jitter between samples */
        UnsignedWide us;
        volatile int spin = (int)(pool[i & 63] & 15);
        while (spin--) ;
        Microseconds(&us);
        stir(&us, sizeof us);
    }
    stir_now();
    while (n) {
        uint8_t out[64], in[68];
        size_t k = n < 32 ? n : 32;
        memcpy(in, pool, 64);
        in[64] = 'o'; in[65] = 'u'; in[66] = 't'; in[67] = (uint8_t)pool_count++;
        g4_sha512(out, in, sizeof in);
        memcpy(buf, out, k);
        buf += k; n -= k;
        stir(out + 32, 32);                     /* never reuse output state */
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* MacTCP: one TCP connection at a time, and a UDP "where are you?" answer */

#pragma pack(push, 2)
typedef struct {                    /* UDP csParam blocks (MacTCP Programmer's Guide) */
    Ptr      rcvBuff;               /* create: 0 */
    unsigned long rcvBuffLen;       /* 4 */
    Ptr      notifyProc;            /* 8 */
    unsigned short localPort;       /* 12 */
    Ptr      userDataPtr;           /* 14 */
    unsigned short endingPort;      /* 18 */
} UDPCreatePB9;
typedef struct {
    unsigned short reserved;        /* send: 0 */
    ip_addr  remoteHost;            /* 2 */
    unsigned short remotePort;      /* 6 */
    Ptr      wdsPtr;                /* 8 */
    Boolean  checkSum;              /* 12 */
    Byte     pad;
    unsigned short sendLength;      /* 14 */
    Ptr      userDataPtr;           /* 16 */
    unsigned short localPort;       /* 20 */
} UDPSendPB9;
typedef struct {
    unsigned short timeOut;         /* receive: 0 */
    ip_addr  remoteHost;            /* 2 */
    unsigned short remotePort;      /* 6 */
    Ptr      rcvBuff;               /* 8 */
    unsigned short rcvBuffLen;      /* 12 */
    unsigned short secondTimeStamp; /* 14 */
    Ptr      userDataPtr;           /* 16 */
    ip_addr  destHost;              /* 20 */
    unsigned short destPort;        /* 24 */
} UDPReceivePB9;
typedef struct {
    QElemPtr qLink;                 /* 0  same header as TCPiopb */
    short    qType;
    short    ioTrap;
    Ptr      ioCmdAddr;
    Ptr      ioCompletion;          /* 12 */
    OSErr    ioResult;              /* 16 */
    StringPtr ioNamePtr;
    short    ioVRefNum;
    short    ioCRefNum;             /* 24 */
    short    csCode;                /* 26 */
    StreamPtr udpStream;            /* 28 */
    union { UDPCreatePB9 create; UDPSendPB9 send; UDPReceivePB9 receive; } csParam;   /* 32 */
} UDPiopb9;
typedef struct { unsigned short length; Ptr ptr; } wds9;
#pragma pack(pop)

enum { kUDPCreate = 20, kUDPRead = 21, kUDPBfrReturn = 22, kUDPWrite = 23, kUDPRelease = 24 };

#define TCP_RCVBUF   (64L * 1024)
#define RX_CHUNK     (32L * 1024)

static short    ip_ref = 0;
static ip_addr  my_ip = 0;
static StreamPtr tcp_stream = 0;
static TCPiopb  open_pb, rcv_pb;
static int      listening = 0, connected = 0, rcv_pending = 0, peer_closed = 0;
static uint8_t *rx_chunk;                       /* last TCPRcv result */
static long     chunk_len = 0, chunk_off = 0;

static StreamPtr udp_stream = 0;
static UDPiopb9 udp_pb;
static int      udp_pending = 0;
static Str255   my_name;

static int net_open(void)
{
    GetAddrParamBlock ap;
    TCPiopb pb;
    if (OpenDriver("\p.IPP", &ip_ref) != noErr) return -1;
    memset(&ap, 0, sizeof ap);
    ap.ioCRefNum = ip_ref;
    ap.csCode = ipctlGetAddr;
    if (PBControlSync((ParmBlkPtr)&ap) == noErr) my_ip = ap.ourAddress;

    rx_chunk = (uint8_t *)NewPtr(RX_CHUNK);
    memset(&pb, 0, sizeof pb);
    pb.ioCRefNum = ip_ref;
    pb.csCode = TCPCreate;
    pb.csParam.create.rcvBuff = NewPtr(TCP_RCVBUF);
    pb.csParam.create.rcvBuffLen = TCP_RCVBUF;
    if (!rx_chunk || !pb.csParam.create.rcvBuff || PBControlSync((ParmBlkPtr)&pb) != noErr) return -1;
    tcp_stream = pb.tcpStream;

    /* discovery: answer "PPCD?" on UDP port 5960 */
    {
        UDPiopb9 u;
        memset(&u, 0, sizeof u);
        u.ioCRefNum = ip_ref;
        u.csCode = kUDPCreate;
        u.csParam.create.rcvBuff = NewPtr(8192);
        u.csParam.create.rcvBuffLen = 8192;
        u.csParam.create.localPort = G4_PORT;
        if (u.csParam.create.rcvBuff && PBControlSync((ParmBlkPtr)&u) == noErr) udp_stream = u.udpStream;
    }
    {
        StringHandle h = GetString(-16413);     /* the name in Sharing Setup / File Sharing */
        if (h && **h) { HLock((Handle)h); memcpy(my_name, *h, (size_t)(**h) + 1); HUnlock((Handle)h); }
        else memcpy(my_name, "\pPowerPC Mac", 12);
    }
    return 0;
}

static void tcp_listen(void)
{
    if (!tcp_stream || listening || connected) return;
    memset(&open_pb, 0, sizeof open_pb);
    open_pb.ioCRefNum = ip_ref;
    open_pb.csCode = TCPPassiveOpen;
    open_pb.tcpStream = tcp_stream;
    open_pb.csParam.open.ulpTimeoutValue = 20;          /* seconds without ACKs: drop it */
    open_pb.csParam.open.ulpTimeoutAction = 1;          /* abort */
    open_pb.csParam.open.validityFlags = timeoutValue | timeoutAction;
    open_pb.csParam.open.commandTimeoutValue = 0;       /* wait for a connection forever */
    open_pb.csParam.open.localPort = G4_PORT;
    PBControlAsync((ParmBlkPtr)&open_pb);
    listening = 1;
}

static void tcp_start_rcv(void)
{
    memset(&rcv_pb, 0, sizeof rcv_pb);
    rcv_pb.ioCRefNum = ip_ref;
    rcv_pb.csCode = TCPRcv;
    rcv_pb.tcpStream = tcp_stream;
    rcv_pb.csParam.receive.commandTimeoutValue = 0;     /* forever */
    rcv_pb.csParam.receive.rcvBuff = (Ptr)rx_chunk;
    rcv_pb.csParam.receive.rcvBuffLen = (unsigned short)RX_CHUNK;
    PBControlAsync((ParmBlkPtr)&rcv_pb);
    rcv_pending = 1;
}

/* a connection arrived? */
static int tcp_accepted(void)
{
    if (!listening || open_pb.ioResult > 0) return 0;
    listening = 0;
    if (open_pb.ioResult != noErr) return 0;            /* failed: listen again later */
    connected = 1;
    peer_closed = 0;
    chunk_len = chunk_off = 0;
    tcp_start_rcv();
    stir(&open_pb.csParam.open.remoteHost, 4);
    return 1;
}

static void tcp_drop(void)
{
    TCPiopb pb;
    if (!connected) return;
    memset(&pb, 0, sizeof pb);
    pb.ioCRefNum = ip_ref;
    pb.csCode = TCPAbort;
    pb.tcpStream = tcp_stream;
    PBControlSync((ParmBlkPtr)&pb);
    while (rcv_pending && rcv_pb.ioResult > 0) SystemTask();   /* the abort completes it */
    rcv_pending = 0;
    connected = 0;
}

int plat_recv(uint8_t *p, size_t cap)
{
    if (!connected) return -1;
    if (chunk_off < chunk_len) {
        long k = chunk_len - chunk_off;
        if (cap == 0) return 0;
        if ((size_t)k > cap) k = (long)cap;
        memcpy(p, rx_chunk + chunk_off, (size_t)k);
        chunk_off += k;
        if (chunk_off == chunk_len && !peer_closed) tcp_start_rcv();
        return (int)k;
    }
    if (peer_closed) return -1;
    if (!rcv_pending || rcv_pb.ioResult > 0) return 0;
    rcv_pending = 0;
    if (rcv_pb.ioResult != noErr) { peer_closed = 1; return -1; }   /* closing or gone */
    chunk_len = rcv_pb.csParam.receive.rcvBuffLen;
    chunk_off = 0;
    {
        UnsignedWide us;                                 /* packet timing: entropy */
        Microseconds(&us);
        stir(&us, sizeof us);
    }
    if (chunk_len == 0) { tcp_start_rcv(); return 0; }
    return plat_recv(p, cap);
}

int plat_send(const uint8_t *p, size_t n)
{
    TCPiopb pb;
    wds9 wds[2];
    if (!connected || peer_closed) return -1;
    while (n) {
        unsigned short k = (unsigned short)(n > 16384 ? 16384 : n);
        wds[0].length = k; wds[0].ptr = (Ptr)p;
        wds[1].length = 0; wds[1].ptr = NULL;
        memset(&pb, 0, sizeof pb);
        pb.ioCRefNum = ip_ref;
        pb.csCode = TCPSend;
        pb.tcpStream = tcp_stream;
        pb.csParam.send.pushFlag = 1;
        pb.csParam.send.wdsPtr = (Ptr)wds;
        if (PBControlSync((ParmBlkPtr)&pb) != noErr) return -1;
        p += k; n -= k;
    }
    return 0;
}

static void udp_read(void)
{
    memset(&udp_pb, 0, sizeof udp_pb);
    udp_pb.ioCRefNum = ip_ref;
    udp_pb.csCode = kUDPRead;
    udp_pb.udpStream = udp_stream;
    udp_pb.csParam.receive.timeOut = 0;                 /* forever */
    PBControlAsync((ParmBlkPtr)&udp_pb);
    udp_pending = 1;
}

/* the Mac app looks for us with a "PPCD?" broadcast; answer with our port
   and name ("PPCD!", u16 port, name in Mac Roman) */
static void udp_poll(void)
{
    if (!udp_stream) return;
    if (!udp_pending) { udp_read(); return; }
    if (udp_pb.ioResult > 0) return;
    udp_pending = 0;
    if (udp_pb.ioResult == noErr) {
        Ptr buf = udp_pb.csParam.receive.rcvBuff;
        unsigned short len = udp_pb.csParam.receive.rcvBuffLen;
        if (len >= 5 && !memcmp(buf, "PPCD?", 5)) {
            uint8_t reply[5 + 2 + 255];
            wds9 wds[2];
            UDPiopb9 s;
            size_t n = 5 + 2 + my_name[0];
            memcpy(reply, "PPCD!", 5);
            g4_put16(reply + 5, G4_PORT);
            memcpy(reply + 7, my_name + 1, my_name[0]);
            wds[0].length = (unsigned short)n; wds[0].ptr = (Ptr)reply;
            wds[1].length = 0; wds[1].ptr = NULL;
            memset(&s, 0, sizeof s);
            s.ioCRefNum = ip_ref;
            s.csCode = kUDPWrite;
            s.udpStream = udp_stream;
            s.csParam.send.remoteHost = udp_pb.csParam.receive.remoteHost;
            s.csParam.send.remotePort = udp_pb.csParam.receive.remotePort;
            s.csParam.send.wdsPtr = (Ptr)wds;
            s.csParam.send.checkSum = 1;
            PBControlSync((ParmBlkPtr)&s);
        }
        {
            UDPiopb9 r;                                  /* give MacTCP its buffer back */
            memset(&r, 0, sizeof r);
            r.ioCRefNum = ip_ref;
            r.csCode = kUDPBfrReturn;
            r.udpStream = udp_stream;
            r.csParam.receive.rcvBuff = buf;
            PBControlSync((ParmBlkPtr)&r);
        }
    }
    udp_read();
}

/* ------------------------------------------------------------------ */
/* the screen                                                          */

static WindowPtr cover = NULL;                  /* full-screen black window while showing */
static short     saved_mbar = 0;
static RgnHandle saved_gray = NULL;
static short     saved_depth = 0;               /* switched to thousands for the session */

static GDHandle main_gd(void) { return GetMainDevice(); }

int plat_screen_info(int *w, int *h, int *depth, int *width_mm)
{
    GDHandle gd = main_gd();
    PixMapHandle pm = (**gd).gdPMap;
    int d = (**pm).pixelSize;
    if (d < 16 && HasDepth(gd, 16, 0, 0)) {             /* 256 colours: thousands for now */
        saved_depth = (short)d;
        if (SetDepth(gd, 16, 0, 0) == noErr) d = (**pm).pixelSize;
    }
    *w = (**gd).gdRect.right - (**gd).gdRect.left;
    *h = (**gd).gdRect.bottom - (**gd).gdRect.top;
    *depth = d;
    *width_mm = 0;
    return 0;
}

static void restore_depth(void)
{
    if (saved_depth) { SetDepth(main_gd(), saved_depth, 0, 0); saved_depth = 0; }
}

int plat_screen_take(rx_screen *s)
{
    GDHandle gd = main_gd();
    PixMapHandle pm = (**gd).gdPMap;
    Rect r = (**gd).gdRect;
    /* hide the menu bar (so its clock can't draw over the picture) and
       cover the whole screen, so nothing else redraws there */
    saved_mbar = LMGetMBarHeight();
    saved_gray = NewRgn();
    CopyRgn(GetGrayRgn(), saved_gray);
    LMSetMBarHeight(0);
    {
        RgnHandle full = NewRgn();
        RectRgn(full, &r);
        UnionRgn(GetGrayRgn(), full, GetGrayRgn());
        DisposeRgn(full);
    }
    cover = NewCWindow(NULL, &r, "\p", true, plainDBox, (WindowPtr)-1L, false, 0);
    if (cover) {
        SetPort((GrafPtr)cover);
        RGBForeColor(&(RGBColor){ 0, 0, 0 });
        PaintRect(&r);
    }
    HideCursor();
    s->base = (uint8_t *)(**pm).baseAddr;
    s->rowbytes = (**pm).rowBytes & 0x3FFF;
    s->width = r.right - r.left;
    s->height = r.bottom - r.top;
    s->depth = (**pm).pixelSize;
    return s->base ? 0 : -1;
}

void plat_screen_release(void)
{
    if (cover) { DisposeWindow(cover); cover = NULL; }
    if (saved_gray) {
        CopyRgn(saved_gray, GetGrayRgn());
        DisposeRgn(saved_gray);
        saved_gray = NULL;
        LMSetMBarHeight(saved_mbar);
    }
    ShowCursor();
    DrawMenuBar();
    /* repaint the desktop and every window */
    PaintBehind((WindowPtr)LMGetWindowList(), GetGrayRgn());
    CalcVisBehind((WindowPtr)LMGetWindowList(), GetGrayRgn());
}

/* ------------------------------------------------------------------ */
/* keyboard and mouse                                                  */

void plat_input(int *x, int *y, int *buttons, unsigned char keys[16])
{
    Point pt;
    KeyMap km;
    GrafPtr old;
    GetPort(&old);
    if (cover) SetPort((GrafPtr)cover);                 /* its origin is the screen's */
    GetMouse(&pt);
    if (!cover) LocalToGlobal(&pt);
    SetPort(old);
    *x = pt.h; *y = pt.v;
    *buttons = Button() ? 1 : 0;
    GetKeys(km);
    memcpy(keys, km, 16);
}

/* ------------------------------------------------------------------ */
/* sound: the Sound Manager, four buffers of up to 2048 frames         */

#define SND_BUFS   4
#define SND_FRAMES 2048
typedef struct { ExtSoundHeader h; int16_t data[SND_FRAMES * 2]; } snd_buf;
static SndChannelPtr chan = NULL;
static snd_buf *bufs = NULL;
static volatile int buf_busy[SND_BUFS];
static int16_t *fifo = NULL;                    /* interleaved stereo waiting for a buffer */
static long fifo_len = 0;
#define FIFO_FRAMES (SND_FRAMES * 6)
static int snd_started = 0, snd_ok = -1;
static unsigned snd_rate = 0;
static SndCallbackUPP snd_cb_upp = NULL;

static pascal void snd_done(SndChannelPtr c, SndCommand *cmd)
{
    (void)c;
    if (cmd->param1 >= 0 && cmd->param1 < SND_BUFS) buf_busy[cmd->param1] = 0;   /* interrupt time: just a flag */
}

static int snd_open(void)
{
    if (snd_ok >= 0) return snd_ok;
    snd_ok = 0;
    bufs = (snd_buf *)NewPtrClear(sizeof(snd_buf) * SND_BUFS);
    fifo = (int16_t *)NewPtr(FIFO_FRAMES * 4);
    snd_cb_upp = NewSndCallbackUPP(snd_done);
    if (!bufs || !fifo || !snd_cb_upp) return 0;
    if (SndNewChannel(&chan, sampledSynth, initStereo, snd_cb_upp) != noErr) { chan = NULL; return 0; }
    snd_ok = 1;
    return 1;
}

int plat_sound_available(void) { return snd_open(); }

static void snd_feed(void)
{
    int i;
    if (!chan) return;
    /* start after ~90 ms are waiting, so the network's jitter doesn't click */
    if (!snd_started && fifo_len < (long)snd_rate * 90 / 1000) return;
    snd_started = 1;
    for (i = 0; i < SND_BUFS && fifo_len > 0; i++) {
        SndCommand c;
        long n;
        if (buf_busy[i]) continue;
        n = fifo_len < SND_FRAMES ? fifo_len : SND_FRAMES;
        memcpy(bufs[i].data, fifo, (size_t)n * 4);
        memmove(fifo, fifo + n * 2, (size_t)(fifo_len - n) * 4);
        fifo_len -= n;
        memset(&bufs[i].h, 0, sizeof bufs[i].h);
        bufs[i].h.samplePtr = (Ptr)bufs[i].data;
        bufs[i].h.numChannels = 2;
        bufs[i].h.sampleRate = (Fixed)((unsigned long)snd_rate << 16);
        bufs[i].h.encode = extSH;
        bufs[i].h.baseFrequency = 60;               /* middle C: play at the recorded rate */
        bufs[i].h.numFrames = n;
        bufs[i].h.sampleSize = 16;
        buf_busy[i] = 1;
        c.cmd = bufferCmd; c.param1 = 0; c.param2 = (long)&bufs[i].h;
        SndDoCommand(chan, &c, true);
        c.cmd = callBackCmd; c.param1 = (short)i; c.param2 = 0;
        SndDoCommand(chan, &c, true);
    }
    if (fifo_len == 0) {
        int any = 0;
        for (i = 0; i < SND_BUFS; i++) any |= buf_busy[i];
        if (!any) snd_started = 0;                   /* ran dry: buffer up again */
    }
}

void plat_sound(const int16_t *pcm, unsigned frames, unsigned ch, unsigned rate)
{
    unsigned i;
    if (!snd_open() || !chan) return;
    if (rate != snd_rate) { snd_rate = rate; fifo_len = 0; snd_started = 0; }
    if (fifo_len + (long)frames > FIFO_FRAMES) return;       /* too far behind: skip this packet */
    for (i = 0; i < frames; i++) {
        fifo[(fifo_len + i) * 2] = pcm[i * ch];
        fifo[(fifo_len + i) * 2 + 1] = ch == 2 ? pcm[i * ch + 1] : pcm[i * ch];
    }
    fifo_len += frames;
    snd_feed();
}

void plat_sound_stop(void)
{
    SndCommand c;
    int i;
    fifo_len = 0;
    snd_started = 0;
    if (!chan) return;
    c.cmd = flushCmd; c.param1 = 0; c.param2 = 0; SndDoImmediate(chan, &c);
    c.cmd = quietCmd; c.param1 = 0; c.param2 = 0; SndDoImmediate(chan, &c);
    for (i = 0; i < SND_BUFS; i++) buf_busy[i] = 0;
}

/* ------------------------------------------------------------------ */
/* the pairing file: "PowerPC Display Prefs" in the Preferences folder */

static int prefs_spec(FSSpec *spec)
{
    short vref;
    long dir;
    OSErr e;
    if (FindFolder(kOnSystemDisk, kPreferencesFolderType, kCreateFolder, &vref, &dir) != noErr) return -1;
    e = FSMakeFSSpec(vref, dir, "\pPowerPC Display Prefs", spec);
    return (e == noErr || e == fnfErr) ? 0 : -1;
}

size_t plat_prefs_load(char *buf, size_t cap)
{
    FSSpec spec;
    short ref;
    long n = (long)cap;
    if (prefs_spec(&spec) || FSpOpenDF(&spec, fsRdPerm, &ref) != noErr) return 0;
    if (FSRead(ref, &n, buf) != noErr && n <= 0) n = 0;
    FSClose(ref);
    return (size_t)n;
}

void plat_prefs_save(const char *buf, size_t n)
{
    FSSpec spec;
    short ref;
    long len = (long)n;
    if (prefs_spec(&spec)) return;
    FSpCreate(&spec, 'PPCD', 'pref', smSystemScript);   /* already there: fine */
    if (FSpOpenDF(&spec, fsRdWrPerm, &ref) != noErr) return;
    SetEOF(ref, 0);
    FSWrite(ref, &len, buf);
    FSClose(ref);
}

/* ------------------------------------------------------------------ */
/* the window                                                          */

enum { mApple = 128, mFile = 129, mEdit = 130 };
enum { iAbout = 1, iForget = 1, iQuit = 3 };

static WindowPtr win = NULL;
static ControlHandle b_pair = NULL, b_nopair = NULL, b_resume = NULL;
static char status_text[400] = "Starting...";
static int status_state = RX_STATE_WAITING;
static int paused = 0, quitting = 0;
static rx *session = NULL;
static char log_line[200] = "";

void plat_log(const char *text)
{
    strncpy(log_line, text, sizeof log_line - 1);
    log_line[sizeof log_line - 1] = 0;
}

static void ip_string(char *out)
{
    sprintf(out, "%lu.%lu.%lu.%lu", (unsigned long)(my_ip >> 24) & 255, (unsigned long)(my_ip >> 16) & 255,
            (unsigned long)(my_ip >> 8) & 255, (unsigned long)my_ip & 255);
}

static void draw_window(void)
{
    Rect r;
    char line[300], ip[20];
    GrafPtr old;
    if (!win) return;
    GetPort(&old);
    SetPort((GrafPtr)win);
    r = win->portRect;
    EraseRect(&r);
    TextFont(0); TextSize(12);
    MoveTo(16, 24);
    DrawString("\pPowerPC Display");
    TextFont(1); TextSize(10);               /* Geneva */
    SetRect(&r, 16, 34, win->portRect.right - 16, 94);
    TETextBox(status_text, (long)strlen(status_text), &r, teFlushLeft);
    ip_string(ip);
    if (ip_ref == 0)
        sprintf(line, "No TCP/IP: check the TCP/IP control panel.");
    else
        sprintf(line, "This Mac: %.*s  -  %s, port %d  -  %d paired Mac%s",
                (int)my_name[0], (const char *)my_name + 1, ip, G4_PORT,
                rx_paired_count(), rx_paired_count() == 1 ? "" : "s");
    MoveTo(16, 110);
    {
        Str255 ps;
        size_t n = strlen(line);
        if (n > 255) n = 255;
        ps[0] = (unsigned char)n; memcpy(ps + 1, line, n);
        DrawString(ps);
    }
    if (log_line[0]) {
        Str255 ps;
        size_t n = strlen(log_line);
        if (n > 80) n = 80;
        ps[0] = (unsigned char)n; memcpy(ps + 1, log_line, n);
        MoveTo(16, 126);
        DrawString(ps);
    }
    DrawControls(win);
    SetPort(old);
}

static void show_buttons(void)
{
    if (!win) return;
    if (status_state == RX_STATE_PAIRING) { ShowControl(b_pair); ShowControl(b_nopair); }
    else { HideControl(b_pair); HideControl(b_nopair); }
    if (paused) ShowControl(b_resume); else HideControl(b_resume);
}

void plat_status(int state, const char *text)
{
    status_state = state;
    strncpy(status_text, text, sizeof status_text - 1);
    status_text[sizeof status_text - 1] = 0;
    if (win) {
        GrafPtr old;
        GetPort(&old);
        SetPort((GrafPtr)win);
        show_buttons();
        InvalRect(&win->portRect);
        SetPort(old);
    }
}

void plat_pair_prompt(const char *text)
{
    plat_status(RX_STATE_PAIRING, text);
    if (win) SelectWindow(win);
    SysBeep(10);
}

int plat_cache_slots(int bpp)
{
    long tile = 32L * 32 * bpp, room = FreeMem() / 3;     /* a third of what's free */
    long slots = room / tile;
    if (slots > G4_DEFAULT_SLOTS) slots = G4_DEFAULT_SLOTS;
    return (int)(slots < 64 ? 0 : slots);
}

static void make_window(void)
{
    Rect r, b;
    SetRect(&r, 40, 60, 40 + 460, 60 + 180);
    win = NewCWindow(NULL, &r, "\pPowerPC Display", true, noGrowDocProc, (WindowPtr)-1L, true, 0);
    SetPort((GrafPtr)win);
    SetRect(&b, 460 - 16 - 100, 180 - 16 - 20, 460 - 16, 180 - 16);
    b_pair = NewControl(win, &b, "\pPair", false, 0, 0, 1, pushButProc, 1);
    OffsetRect(&b, -112, 0);
    b_nopair = NewControl(win, &b, "\pDon't Pair", false, 0, 0, 1, pushButProc, 2);
    SetRect(&b, 460 - 16 - 100, 180 - 16 - 20, 460 - 16, 180 - 16);
    b_resume = NewControl(win, &b, "\pResume", false, 0, 0, 1, pushButProc, 3);
}

static void make_menus(void)
{
    MenuHandle m = NewMenu(mApple, "\p\024");
    AppendMenu(m, "\pAbout PowerPC Display...;(-");
    AppendResMenu(m, 'DRVR');
    InsertMenu(m, 0);
    m = NewMenu(mFile, "\pFile");
    AppendMenu(m, "\pForget Paired Macs;(-;Quit/Q");
    InsertMenu(m, 0);
    m = NewMenu(mEdit, "\pEdit");
    AppendMenu(m, "\pUndo/Z;(-;Cut/X;Copy/C;Paste/V;Clear");
    InsertMenu(m, 0);
    DrawMenuBar();
}

static void end_session(int why)
{
    rx_end(session);
    session = NULL;
    tcp_drop();
    restore_depth();
    if (why == RX_USER_LEFT) {
        paused = 1;
        plat_status(RX_STATE_WAITING, "Paused: your Mac can't connect. Click Resume to let it show its screen again.");
    } else if (why == RX_HOST_SLEEPS) {
        plat_status(RX_STATE_WAITING, "Your Mac went to sleep. It reconnects when it wakes.");
    } else if (status_state == RX_STATE_CONNECTED || status_state == RX_STATE_PAIRING) {
        plat_status(RX_STATE_WAITING, "Disconnected. Waiting for your Mac...");
    }
    if (win) SelectWindow(win);
}

static void do_menu(long choice)
{
    short menu = (short)(choice >> 16), item = (short)(choice & 0xFFFF);
    if (menu == mApple) {
        if (item == iAbout) {
            plat_status(status_state, "PowerPC Display " RX_VERSION " for Mac OS 9: shows your Mac's screen here, "
                        "with this Mac's keyboard, mouse and speakers. Open PowerPC Display on your Mac to connect.");
        } else {
            Str255 name;
            GetMenuItemText(GetMenuHandle(mApple), item, name);
            OpenDeskAcc(name);
        }
    } else if (menu == mFile) {
        if (item == iForget) {
            rx_forget_pairings();
            plat_status(status_state, "Forgot every paired Mac. Each one will ask to pair again.");
        } else if (item == iQuit) quitting = 1;
    } else if (menu == mEdit) {
        SystemEdit(item - 1);
    }
    HiliteMenu(0);
}

static void do_mouse(EventRecord *ev)
{
    WindowPtr w;
    short part = FindWindow(ev->where, &w);
    if (part == inMenuBar) do_menu(MenuSelect(ev->where));
    else if (part == inSysWindow) SystemClick(ev, w);
    else if (part == inDrag) DragWindow(w, ev->where, &qd.screenBits.bounds);
    else if (part == inGoAway) { if (TrackGoAway(w, ev->where)) quitting = 1; }
    else if (part == inContent && w == win) {
        ControlHandle c;
        Point pt = ev->where;
        if (FrontWindow() != win) { SelectWindow(win); return; }
        SetPort((GrafPtr)win);
        GlobalToLocal(&pt);
        if (FindControl(pt, win, &c) && TrackControl(c, pt, NULL)) {
            if (c == b_pair && session) rx_pair_answer(session, 1);
            else if (c == b_nopair && session) rx_pair_answer(session, 0);
            else if (c == b_resume) {
                paused = 0;
                plat_status(RX_STATE_WAITING, "Waiting for your Mac...");
            }
        }
    }
}

static pascal OSErr ae_quit(const AppleEvent *e, AppleEvent *reply, long refcon)
{
    (void)e; (void)reply; (void)refcon;
    quitting = 1;
    return noErr;
}

int main(void)
{
    EventRecord ev;

    MaxApplZone();
    MoreMasters(); MoreMasters();
    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(NULL);
    InitCursor();
    FlushEvents(everyEvent, 0);
    AEInstallEventHandler(kCoreEventClass, kAEQuitApplication, NewAEEventHandlerUPP(ae_quit), 0, false);

    make_menus();
    make_window();
    stir_now();
    if (net_open()) {
        ip_ref = 0;
        plat_status(RX_STATE_ERROR, "Couldn't start TCP/IP. Check the TCP/IP control panel (Ethernet), then open PowerPC Display again.");
    } else {
        plat_status(RX_STATE_WAITING, "Waiting for your Mac. Open PowerPC Display on it: it finds this Mac by itself.");
    }
    rx_init();

    while (!quitting) {
        long sleep = (session && rx_showing(session)) ? 0 : 2;
        if (WaitNextEvent(everyEvent, &ev, sleep, NULL)) {
            stir(&ev, sizeof ev);
            switch (ev.what) {
            case mouseDown: if (!session || !rx_showing(session)) do_mouse(&ev); break;
            case keyDown:
            case autoKey:
                if ((ev.modifiers & cmdKey) && (!session || !rx_showing(session))) do_menu(MenuKey((char)(ev.message & charCodeMask)));
                break;
            case updateEvt:
                BeginUpdate((WindowPtr)ev.message);
                if ((WindowPtr)ev.message == win) draw_window();
                EndUpdate((WindowPtr)ev.message);
                break;
            case kHighLevelEvent:
                AEProcessAppleEvent(&ev);
                break;
            default:
                break;
            }
        }
        if (!ip_ref) continue;
        udp_poll();
        if (!session) {
            if (!paused) tcp_listen();
            if (tcp_accepted()) {
                if (paused) tcp_drop();
                else {
                    session = rx_begin();
                    if (!session) { tcp_drop(); restore_depth(); }
                }
            }
        } else {
            int rc = rx_poll(session);
            snd_feed();
            if (rc != RX_GOING) end_session(rc);
        }
    }
    if (session) end_session(RX_ENDED);
    if (listening) {                                    /* stop waiting for connections */
        TCPiopb pb;
        memset(&pb, 0, sizeof pb);
        pb.ioCRefNum = ip_ref; pb.csCode = TCPAbort; pb.tcpStream = tcp_stream;
        PBControlSync((ParmBlkPtr)&pb);
    }
    if (tcp_stream) {
        TCPiopb pb;
        memset(&pb, 0, sizeof pb);
        pb.ioCRefNum = ip_ref; pb.csCode = TCPRelease; pb.tcpStream = tcp_stream;
        PBControlSync((ParmBlkPtr)&pb);
    }
    if (udp_stream) {
        UDPiopb9 u;
        memset(&u, 0, sizeof u);
        u.ioCRefNum = ip_ref; u.csCode = kUDPRelease; u.udpStream = udp_stream;
        PBControlSync((ParmBlkPtr)&u);
    }
    if (chan) SndDisposeChannel(chan, true);
    return 0;
}
