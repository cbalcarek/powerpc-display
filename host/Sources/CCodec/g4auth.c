/* g4auth.c — see g4auth.h. Plain C99 (gcc 4.0 on Tiger, clang on macOS 14+). */
#include "g4auth.h"
#include "g4nacl.h"

#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int g4_random(uint8_t *buf, size_t n)
{
    int fd = open("/dev/urandom", O_RDONLY);
    size_t got = 0;
    if (fd < 0) return -1;
    while (got < n) {
        ssize_t r = read(fd, buf + got, n - got);
        if (r <= 0) { close(fd); return -1; }
        got += (size_t)r;
    }
    close(fd);
    return 0;
}

void randombytes(unsigned char *x, unsigned long long n)
{
    if (g4_random(x, (size_t)n)) memset(x, 0, (size_t)n);   /* never used by our code paths */
}

int g4_dh_keypair(uint8_t pub[G4_PUB_BYTES], uint8_t priv[G4_PUB_BYTES])
{
    if (g4_random(priv, G4_PUB_BYTES)) return -1;
    crypto_scalarmult_base(pub, priv);
    return 0;
}

#define SHA512_BLOCK 128

void g4_hmac_sha512(uint8_t out[64], const uint8_t *key, size_t klen, const uint8_t *msg, size_t mlen)
{
    uint8_t k[SHA512_BLOCK], inner[64], buf[SHA512_BLOCK + 64];
    uint8_t *ibuf;
    size_t i;
    memset(k, 0, sizeof k);
    if (klen > SHA512_BLOCK) crypto_hash(k, key, klen); else memcpy(k, key, klen);
    ibuf = (uint8_t *)malloc(SHA512_BLOCK + mlen);
    if (!ibuf) { memset(out, 0, 64); return; }
    for (i = 0; i < SHA512_BLOCK; i++) ibuf[i] = k[i] ^ 0x36;
    memcpy(ibuf + SHA512_BLOCK, msg, mlen);
    crypto_hash(inner, ibuf, SHA512_BLOCK + mlen);
    free(ibuf);
    for (i = 0; i < SHA512_BLOCK; i++) buf[i] = k[i] ^ 0x5c;
    memcpy(buf + SHA512_BLOCK, inner, 64);
    crypto_hash(out, buf, SHA512_BLOCK + 64);
}

int g4_pair_key(uint8_t key[G4_KEY_BYTES], const uint8_t priv[G4_PUB_BYTES],
                const uint8_t peer_pub[G4_PUB_BYTES],
                const uint8_t client_pub[G4_PUB_BYTES], const uint8_t host_pub[G4_PUB_BYTES],
                const uint8_t client_id[G4_ID_BYTES], const uint8_t host_id[G4_ID_BYTES])
{
    uint8_t shared[32], zero[32], m[8 + 32 + 32 + 32 + 16 + 16], h[64];
    memset(zero, 0, sizeof zero);
    crypto_scalarmult(shared, priv, peer_pub);
    if (g4_equal(shared, zero, 32)) return -1;              /* low-order peer key */
    memcpy(m, "PPCDpair", 8);
    memcpy(m + 8, shared, 32);
    memcpy(m + 40, client_pub, 32);
    memcpy(m + 72, host_pub, 32);
    memcpy(m + 104, client_id, 16);
    memcpy(m + 120, host_id, 16);
    crypto_hash(h, m, sizeof m);
    memcpy(key, h, G4_KEY_BYTES);
    memset(shared, 0, sizeof shared); memset(m, 0, sizeof m); memset(h, 0, sizeof h);
    return 0;
}

uint32_t g4_pair_code(const uint8_t key[G4_KEY_BYTES])
{
    uint8_t h[64];
    uint64_t v = 0;
    int i;
    g4_hmac_sha512(h, key, G4_KEY_BYTES, (const uint8_t *)"PPCDcode", 8);
    for (i = 0; i < 8; i++) v = (v << 8) | h[i];
    return (uint32_t)(v % 1000000u);
}

void g4_auth_tag(uint8_t tag[G4_TAG_BYTES], const uint8_t key[G4_KEY_BYTES], char who,
                 const uint8_t client_id[G4_ID_BYTES], const uint8_t host_id[G4_ID_BYTES],
                 const uint8_t client_nonce[G4_NONCE_BYTES], const uint8_t host_nonce[G4_NONCE_BYTES])
{
    uint8_t m[8 + 16 * 4], h[64];
    memcpy(m, "PPCDauth", 8);
    m[7] = (uint8_t)who;
    memcpy(m + 8, client_id, 16);
    memcpy(m + 24, host_id, 16);
    memcpy(m + 40, client_nonce, 16);
    memcpy(m + 56, host_nonce, 16);
    g4_hmac_sha512(h, key, G4_KEY_BYTES, m, sizeof m);
    memcpy(tag, h, G4_TAG_BYTES);
}

int g4_equal(const uint8_t *a, const uint8_t *b, size_t n)
{
    uint8_t d = 0;
    size_t i;
    for (i = 0; i < n; i++) d |= (uint8_t)(a[i] ^ b[i]);
    return d == 0;
}

void g4_hex(const uint8_t *in, size_t n, char *out)
{
    static const char *hx = "0123456789abcdef";
    size_t i;
    for (i = 0; i < n; i++) { out[2 * i] = hx[in[i] >> 4]; out[2 * i + 1] = hx[in[i] & 15]; }
    out[2 * n] = 0;
}

static int nib(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int g4_unhex(const char *in, uint8_t *out, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        int a = nib(in[2 * i]), b;
        if (a < 0) return -1;
        b = nib(in[2 * i + 1]);
        if (b < 0) return -1;
        out[i] = (uint8_t)(a << 4 | b);
    }
    return 0;
}
