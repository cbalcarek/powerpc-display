/*
 * test_auth.c — pairing/authentication tests: TweetNaCl against RFC 7748 and
 * SHA-512 vectors, HMAC-SHA512 against RFC 4231, then full pairing and
 * reconnect handshakes including a man in the middle and replayed tags.
 */
#include "g4auth.h"
#include "../host/Sources/CCodec/g4nacl.h"

#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); fails++; } } while (0)

static void unhex(const char *s, uint8_t *o) { g4_unhex(s, o, strlen(s) / 2); }

int main(void)
{
    uint8_t a[32], b[32], pa[32], pb[32], sa[32], exp[64], h[64];
    /* vectors */
    unhex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a", a);
    unhex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb", b);
    crypto_scalarmult_base(pa, a); crypto_scalarmult_base(pb, b);
    crypto_scalarmult(sa, a, pb);
    unhex("4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742", exp);
    CHECK(!memcmp(sa, exp, 32), "X25519 RFC 7748 shared secret");
    crypto_hash(h, (const uint8_t *)"abc", 3);
    unhex("ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f", exp);
    CHECK(!memcmp(h, exp, 64), "SHA-512 abc");
    g4_hmac_sha512(h, (const uint8_t *)"Jefe", 4, (const uint8_t *)"what do ya want for nothing?", 28);
    unhex("164b7a7bfcf819e2e395fbe73b56e0a387bd64222e831fd610270cd7ea2505549758bf75c05a994a6d034f65f8f0e6fdcaeab1a34d4a6b4b636e070a38bce737", exp);
    CHECK(!memcmp(h, exp, 64), "HMAC-SHA512 RFC 4231 case 2");
    {   /* RFC 4231 case 6: 131-byte key (longer than a block) */
        uint8_t k[131]; memset(k, 0xaa, sizeof k);
        g4_hmac_sha512(h, k, sizeof k, (const uint8_t *)"Test Using Larger Than Block-Size Key - Hash Key First", 54);
        unhex("80b24263c7c1a3ebb71493c1dd7be8b49b46d1f41b4aeec1121b013783f8f3526b56d037e05f2598bd0fd2215d6a1e5295e64f73f63f0aec8b915a985d786598", exp);
        CHECK(!memcmp(h, exp, 64), "HMAC-SHA512 RFC 4231 case 6");
    }

    /* pairing: client (PowerPC) and host (Apple Silicon) */
    {
        uint8_t cid[16], hid[16], cpub[32], cpriv[32], hpub[32], hpriv[32], kc[32], kh[32];
        uint8_t nc[16], nh[16], th[32], tc[32], t2[32];
        uint8_t mpub[32], mpriv[32], kc_m[32], kh_m[32];
        int i, distinct = 0;
        uint32_t codes[50];
        g4_random(cid, 16); g4_random(hid, 16);
        CHECK(!g4_dh_keypair(cpub, cpriv) && !g4_dh_keypair(hpub, hpriv), "keypairs");
        CHECK(!g4_pair_key(kc, cpriv, hpub, cpub, hpub, cid, hid), "client key");
        CHECK(!g4_pair_key(kh, hpriv, cpub, cpub, hpub, cid, hid), "host key");
        CHECK(!memcmp(kc, kh, 32), "both sides derive the same key");
        CHECK(g4_pair_code(kc) == g4_pair_code(kh) && g4_pair_code(kc) < 1000000, "same 6-digit code");
        printf("example pairing code: %06u\n", (unsigned)g4_pair_code(kc));

        /* man in the middle: substitutes its own public key both ways */
        g4_dh_keypair(mpub, mpriv);
        g4_pair_key(kc_m, cpriv, mpub, cpub, mpub, cid, hid);     /* client thinks mpub is the host */
        g4_pair_key(kh_m, hpriv, mpub, mpub, hpub, cid, hid);     /* host thinks mpub is the client */
        CHECK(memcmp(kc_m, kh_m, 32) != 0, "MITM: keys differ");
        CHECK(g4_pair_code(kc_m) != g4_pair_code(kh_m), "MITM: codes differ (user would notice)");

        /* low-order (all-zero) public key is rejected */
        { uint8_t z[32]; memset(z, 0, 32); CHECK(g4_pair_key(kc_m, cpriv, z, cpub, z, cid, hid) == -1, "zero key rejected"); }

        /* reconnect: mutual tags over fresh nonces */
        g4_random(nc, 16); g4_random(nh, 16);
        g4_auth_tag(th, kh, 'H', cid, hid, nc, nh);
        g4_auth_tag(t2, kc, 'H', cid, hid, nc, nh);
        CHECK(g4_equal(th, t2, 32), "client verifies host tag");
        g4_auth_tag(tc, kc, 'C', cid, hid, nc, nh);
        CHECK(!g4_equal(tc, th, 32), "client tag differs from host tag (no reflection)");
        g4_auth_tag(t2, kc, 'H', cid, hid, nc, nh); t2[0] ^= 1;
        CHECK(!g4_equal(th, t2, 32), "tampered tag rejected");
        { uint8_t nc2[16]; memcpy(nc2, nc, 16); nc2[3] ^= 0x40;
          g4_auth_tag(t2, kh, 'H', cid, hid, nc2, nh);
          CHECK(!g4_equal(th, t2, 32), "replay with a new client nonce fails"); }
        { uint8_t wrong[32]; memcpy(wrong, kh, 32); wrong[31] ^= 1;
          g4_auth_tag(t2, wrong, 'H', cid, hid, nc, nh);
          CHECK(!g4_equal(th, t2, 32), "wrong key rejected"); }

        /* codes look random */
        for (i = 0; i < 50; i++) { uint8_t k[32]; g4_random(k, 32); codes[i] = g4_pair_code(k); }
        for (i = 1; i < 50; i++) if (codes[i] != codes[0]) distinct++;
        CHECK(distinct > 45, "codes vary");
    }
    {   /* hex round trip */
        uint8_t in[16], out[16]; char hx[33];
        g4_random(in, 16); g4_hex(in, 16, hx);
        CHECK(strlen(hx) == 32 && !g4_unhex(hx, out, 16) && !memcmp(in, out, 16), "hex round trip");
        CHECK(g4_unhex("zz", out, 1) == -1, "bad hex rejected");
    }
    printf(fails ? "AUTH TESTS FAILED (%d)\n" : "AUTH TESTS PASSED\n", fails);
    return fails != 0;
}
