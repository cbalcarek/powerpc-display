/*
 * Bonjour discovery for g4host: browse -> resolve -> IPv4 address lookup,
 * all with Apple's dns_sd API (part of libSystem on macOS).
 */
#include "g4discovery.h"

#include <arpa/inet.h>
#include <dns_sd.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <unistd.h>

#define MAX_CANDS 8

typedef struct {
    char     name[256], regtype[256], domain[256];
    uint32_t ifs[8];
    int      nifs;
    int      gone;          /* the network said it went away */
} cand;

typedef struct {
    const char *filter;
    int         done;      /* step finished (success) */
    int         failed;
    int         err;
    /* browse results: every PowerPC Mac seen (a VM that was shut down can
       linger in the cache for a long time, so don't trust just the first),
       each possibly on several network interfaces */
    cand        c[MAX_CANDS];
    int         ncand;
    /* resolve result */
    char        host[1025];
    uint16_t    port;
    /* address result */
    char        ip[64];
} disc_ctx;

static long long now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/* pump one DNSServiceRef until ctx->done/failed or the deadline passes */
static int pump(DNSServiceRef ref, disc_ctx *c, long long deadline)
{
    int fd = DNSServiceRefSockFD(ref);
    if (fd < 0) return -1;
    while (!c->done && !c->failed) {
        long long left = deadline - now_ms();
        fd_set rf;
        struct timeval tv;
        int n;
        if (left <= 0) return -1;
        FD_ZERO(&rf); FD_SET(fd, &rf);
        tv.tv_sec = (long)(left / 1000);
        tv.tv_usec = (int)((left % 1000) * 1000);
        n = select(fd + 1, &rf, NULL, NULL, &tv);
        if (n < 0) return -1;
        if (n == 0) return -1;
        if (DNSServiceProcessResult(ref) != kDNSServiceErr_NoError) return -1;
    }
    return c->done ? 0 : -1;
}

static void DNSSD_API browse_cb(DNSServiceRef ref, DNSServiceFlags flags, uint32_t ifindex,
                                DNSServiceErrorType err, const char *name, const char *regtype,
                                const char *domain, void *ctx)
{
    disc_ctx *c = (disc_ctx *)ctx;
    int i;
    (void)ref;
    if (err != kDNSServiceErr_NoError) { c->failed = 1; c->err = err; return; }
    if (c->filter && strcmp(c->filter, name) != 0) return;
    for (i = 0; i < c->ncand; i++) if (!strcmp(c->c[i].name, name)) break;
    if (!(flags & kDNSServiceFlagsAdd)) {          /* removed */
        if (i < c->ncand) c->c[i].gone = 1;
        return;
    }
    if (i == c->ncand) {
        cand *n;
        if (c->ncand == MAX_CANDS) return;
        n = &c->c[c->ncand++];
        memset(n, 0, sizeof *n);
        snprintf(n->name, sizeof n->name, "%s", name);
        snprintf(n->regtype, sizeof n->regtype, "%s", regtype);
        snprintf(n->domain, sizeof n->domain, "%s", domain);
    }
    c->c[i].gone = 0;
    if (c->c[i].nifs < 8) c->c[i].ifs[c->c[i].nifs++] = ifindex;
    c->done = 1;
}

static void DNSSD_API resolve_cb(DNSServiceRef ref, DNSServiceFlags flags, uint32_t ifindex,
                                 DNSServiceErrorType err, const char *fullname, const char *host,
                                 uint16_t port_be, uint16_t txtlen, const unsigned char *txt, void *ctx)
{
    disc_ctx *c = (disc_ctx *)ctx;
    (void)ref; (void)flags; (void)ifindex; (void)fullname; (void)txtlen; (void)txt;
    if (err != kDNSServiceErr_NoError) { c->failed = 1; c->err = err; return; }
    snprintf(c->host, sizeof c->host, "%s", host);
    c->port = ntohs(port_be);
    c->done = 1;
}

static void DNSSD_API addr_cb(DNSServiceRef ref, DNSServiceFlags flags, uint32_t ifindex,
                              DNSServiceErrorType err, const char *host, const struct sockaddr *sa,
                              uint32_t ttl, void *ctx)
{
    disc_ctx *c = (disc_ctx *)ctx;
    (void)ref; (void)flags; (void)ifindex; (void)host; (void)ttl;
    if (err != kDNSServiceErr_NoError) { c->failed = 1; c->err = err; return; }
    if (!sa || sa->sa_family != AF_INET) return;
    if (!inet_ntop(AF_INET, &((const struct sockaddr_in *)sa)->sin_addr, c->ip, sizeof c->ip)) return;
    c->done = 1;
}

/* resolve + address lookup for one candidate; 0 = ok, fills out */
static int try_candidate(disc_ctx *c, const cand *k, g4_found *out)
{
    DNSServiceRef ref = NULL;
    DNSServiceErrorType e;
    long long deadline;
    uint32_t tries[9], ifindex = k->nifs ? k->ifs[0] : 0;
    int ntries = 0, i, ok = 0, rc;

    snprintf(out->name, sizeof out->name, "%s", k->name);
    out->ifindex = ifindex;
    out->host[0] = 0; out->ip[0] = 0; out->port = 0;

    /* 2. resolve name -> host + port: each interface it was seen on, then any */
    out->failed_step = 2;
    out->dns_error = 0;
    for (i = 0; i < k->nifs; i++) tries[ntries++] = k->ifs[i];
    tries[ntries++] = 0;
    for (i = 0; i < ntries && !ok; i++) {
        c->done = c->failed = 0; c->err = 0;
        deadline = now_ms() + (i == ntries - 1 ? 1500 : 1000);
        e = DNSServiceResolve(&ref, 0, tries[i], k->name, k->regtype, k->domain, resolve_cb, c);
        if (e != kDNSServiceErr_NoError) { out->dns_error = e; continue; }
        rc = pump(ref, c, deadline);
        DNSServiceRefDeallocate(ref);
        if (rc == 0) { ok = 1; if (tries[i]) ifindex = tries[i]; }
        else out->dns_error = c->err;
    }
    if (!ok) return -1;
    out->ifindex = ifindex;
    snprintf(out->host, sizeof out->host, "%s", c->host);
    out->port = c->port;

    /* 3. host -> IPv4 on the same interface */
    out->failed_step = 3;
    c->done = c->failed = 0; c->err = 0;
    deadline = now_ms() + 3000;
    e = DNSServiceGetAddrInfo(&ref, 0, ifindex, kDNSServiceProtocol_IPv4, c->host, addr_cb, c);
    if (e != kDNSServiceErr_NoError) { out->dns_error = e; return -1; }
    rc = pump(ref, c, deadline);
    DNSServiceRefDeallocate(ref);
    if (rc) { out->dns_error = c->err; return -1; }
    snprintf(out->ip, sizeof out->ip, "%s", c->ip);
    out->failed_step = 0;
    return 0;
}

int g4_discover(const char *name_filter, int timeout_ms, g4_found *out)
{
    disc_ctx *c = (disc_ctx *)calloc(1, sizeof(disc_ctx));
    DNSServiceRef ref = NULL;
    long long deadline = now_ms() + timeout_ms;
    DNSServiceErrorType e;
    int rc, i, best = -1;
    g4_found first_fail;

    memset(out, 0, sizeof *out);
    if (!c) { out->failed_step = 1; return -1; }
    c->filter = name_filter;

    /* 1. browse; after the first answer keep listening briefly for others */
    out->failed_step = 1;
    e = DNSServiceBrowse(&ref, 0, 0, G4_BONJOUR_TYPE, NULL, browse_cb, c);
    if (e != kDNSServiceErr_NoError) { out->dns_error = e; free(c); return -1; }
    rc = pump(ref, c, deadline);
    if (rc == 0) {
        long long until = now_ms() + 400;
        int fd = DNSServiceRefSockFD(ref);
        while (fd >= 0 && now_ms() < until) {
            fd_set rf;
            struct timeval tv = { 0, 50000 };
            FD_ZERO(&rf); FD_SET(fd, &rf);
            if (select(fd + 1, &rf, NULL, NULL, &tv) > 0 &&
                DNSServiceProcessResult(ref) != kDNSServiceErr_NoError) break;
        }
    }
    DNSServiceRefDeallocate(ref);
    if (rc) { out->dns_error = c->err; free(c); return -1; }

    /* 2+3. try every candidate that hasn't announced it left; report the
       first one that works, or the first failure */
    memset(&first_fail, 0, sizeof first_fail);
    for (i = 0; i < c->ncand; i++) {
        g4_found f;
        if (c->c[i].gone) continue;
        memset(&f, 0, sizeof f);
        if (try_candidate(c, &c->c[i], &f) == 0) { *out = f; best = i; break; }
        /* keep the failure that got furthest (an address-lookup failure still
           gives the caller a host name to try) */
        if (!first_fail.name[0] || f.failed_step > first_fail.failed_step) first_fail = f;
    }
    free(c);
    if (best >= 0) return 0;
    if (first_fail.name[0]) *out = first_fail;
    else out->failed_step = 1;      /* every candidate said goodbye */
    return -1;
}

/* ---------- Wake-on-LAN ---------- */

int g4_wake_on_lan(const uint8_t mac[6])
{
    uint8_t pkt[102];
    struct ifaddrs *ifs, *i;
    int sent = 0, k;
    memset(pkt, 0xFF, 6);
    for (k = 0; k < 16; k++) memcpy(pkt + 6 + 6 * k, mac, 6);
    if (getifaddrs(&ifs) != 0) return 0;
    for (i = ifs; i; i = i->ifa_next) {
        int s, one = 1, p;
        static const uint16_t ports[2] = { 9, 7 };
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
        if (!(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK) || !(i->ifa_flags & IFF_BROADCAST)) continue;
        s = socket(AF_INET, SOCK_DGRAM, 0);
        if (s < 0) continue;
        setsockopt(s, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
#ifdef IP_BOUND_IF
        {
            unsigned idx = if_nametoindex(i->ifa_name);
            if (idx) setsockopt(s, IPPROTO_IP, IP_BOUND_IF, &idx, sizeof idx);   /* out of this interface */
        }
#endif
        for (p = 0; p < 2; p++) {
            struct sockaddr_in to;
            memset(&to, 0, sizeof to);
            to.sin_family = AF_INET;
            to.sin_port = htons(ports[p]);
            /* the interface's own broadcast address (e.g. 169.254.255.255 on a
               direct cable), and the all-ones broadcast */
            if (i->ifa_broadaddr && i->ifa_broadaddr->sa_family == AF_INET) {
                to.sin_addr = ((struct sockaddr_in *)i->ifa_broadaddr)->sin_addr;
                if (sendto(s, pkt, sizeof pkt, 0, (struct sockaddr *)&to, sizeof to) == (ssize_t)sizeof pkt && p == 0) sent++;
            }
            to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
            (void)sendto(s, pkt, sizeof pkt, 0, (struct sockaddr *)&to, sizeof to);
        }
        close(s);
    }
    freeifaddrs(ifs);
    return sent;
}
