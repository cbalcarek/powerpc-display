/*
 * Bonjour discovery for g4host: browse -> resolve -> IPv4 address lookup,
 * all with Apple's dns_sd API (part of libSystem on macOS).
 */
#include "g4discovery.h"

#include <arpa/inet.h>
#include <dns_sd.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/socket.h>

typedef struct {
    const char *filter;
    int         done;      /* step finished (success) */
    int         failed;
    int         err;
    /* browse result */
    char        name[256], regtype[256], domain[256];
    uint32_t    ifindex;
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
    (void)ref;
    if (err != kDNSServiceErr_NoError) { c->failed = 1; c->err = err; return; }
    if (!(flags & kDNSServiceFlagsAdd) || c->done) return;
    if (c->filter && strcmp(c->filter, name) != 0) return;
    snprintf(c->name, sizeof c->name, "%s", name);
    snprintf(c->regtype, sizeof c->regtype, "%s", regtype);
    snprintf(c->domain, sizeof c->domain, "%s", domain);
    c->ifindex = ifindex;
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

int g4_discover(const char *name_filter, int timeout_ms, g4_found *out)
{
    disc_ctx c;
    DNSServiceRef ref = NULL;
    long long deadline = now_ms() + timeout_ms;
    DNSServiceErrorType e;
    int rc;

    memset(&c, 0, sizeof c);
    memset(out, 0, sizeof *out);
    c.filter = name_filter;

    /* 1. browse */
    out->failed_step = 1;
    e = DNSServiceBrowse(&ref, 0, 0, G4_BONJOUR_TYPE, NULL, browse_cb, &c);
    if (e != kDNSServiceErr_NoError) { out->dns_error = e; return -1; }
    rc = pump(ref, &c, deadline);
    DNSServiceRefDeallocate(ref);
    if (rc) { out->dns_error = c.err; return -1; }
    snprintf(out->name, sizeof out->name, "%s", c.name);
    out->ifindex = c.ifindex;

    /* 2. resolve name -> host + port */
    out->failed_step = 2;
    c.done = c.failed = 0; c.err = 0;
    deadline = now_ms() + 4000;
    e = DNSServiceResolve(&ref, 0, c.ifindex, c.name, c.regtype, c.domain, resolve_cb, &c);
    if (e != kDNSServiceErr_NoError) { out->dns_error = e; return -1; }
    rc = pump(ref, &c, deadline);
    DNSServiceRefDeallocate(ref);
    if (rc) { out->dns_error = c.err; return -1; }
    snprintf(out->host, sizeof out->host, "%s", c.host);
    out->port = c.port;

    /* 3. host -> IPv4 on the same interface */
    out->failed_step = 3;
    c.done = c.failed = 0; c.err = 0;
    deadline = now_ms() + 3000;
    e = DNSServiceGetAddrInfo(&ref, 0, c.ifindex, kDNSServiceProtocol_IPv4, c.host, addr_cb, &c);
    if (e != kDNSServiceErr_NoError) { out->dns_error = e; return -1; }
    rc = pump(ref, &c, deadline);
    DNSServiceRefDeallocate(ref);
    if (rc) { out->dns_error = c.err; return -1; }

    snprintf(out->ip, sizeof out->ip, "%s", c.ip);
    out->failed_step = 0;
    return 0;
}
