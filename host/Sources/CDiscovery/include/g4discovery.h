#ifndef G4DISCOVERY_H
#define G4DISCOVERY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define G4_BONJOUR_TYPE "_g4display._tcp"

typedef struct {
    char     name[256];     /* Bonjour service name (the G4's computer name) */
    char     host[1025];    /* e.g. "iMac-G4.local."                         */
    char     ip[64];        /* IPv4 address on the interface it was found on */
    uint16_t port;
    uint32_t ifindex;       /* interface to bind the connection to           */
    /* on failure: which step failed and the dns_sd error (0 = timed out) */
    int      failed_step;   /* 1 browse, 2 resolve, 3 address lookup         */
    int      dns_error;
} g4_found;

/*
 * Browse for a g4display client, resolve it, and look up its IPv4 address on
 * the interface it was seen on (important for direct cables using
 * self-assigned 169.254.x.x addresses). If `name_filter` is non-NULL only a
 * service with that name matches. Returns 0 on success, -1 on timeout/error
 * (see failed_step / dns_error). If only the address lookup fails, `host` and
 * `port` are still filled in, so the caller can fall back to the system
 * resolver.
 */
int g4_discover(const char *name_filter, int timeout_ms, g4_found *out);

#ifdef __cplusplus
}
#endif
#endif
