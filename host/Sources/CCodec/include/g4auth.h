/*
 * g4auth.h — pairing and authentication, shared by both Macs.
 *
 * Pairing (once per pair of Macs): both sides make a fresh X25519 key pair
 * and exchange the public halves. Each side derives the same 32-byte key and
 * the same 6-digit code from it. The user checks that both screens show the
 * same code (like Bluetooth "numeric comparison"), which defeats someone in
 * the middle. Both Macs then store the key.
 *
 * Every later connection: each side proves it holds the key with an
 * HMAC-SHA512 tag over fresh random nonces from both sides. Screen data is
 * still sent unencrypted (encryption would cost the PowerPC too much CPU);
 * pairing means only your Macs can receive your screen or drive the display.
 */
#ifndef G4AUTH_H
#define G4AUTH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define G4_ID_BYTES     16   /* random per installation, not secret */
#define G4_NONCE_BYTES  16
#define G4_PUB_BYTES    32
#define G4_KEY_BYTES    32
#define G4_TAG_BYTES    32

/* 0 = ok, -1 = no randomness available */
int      g4_random(uint8_t *buf, size_t n);
/* fresh X25519 key pair (private key from g4_random); 0 = ok */
int      g4_dh_keypair(uint8_t pub[G4_PUB_BYTES], uint8_t priv[G4_PUB_BYTES]);
/* shared pairing key from our private key and the peer's public key, bound
   to both public keys and both installation IDs. 0 = ok, -1 = bad peer key */
int      g4_pair_key(uint8_t key[G4_KEY_BYTES], const uint8_t priv[G4_PUB_BYTES],
                     const uint8_t peer_pub[G4_PUB_BYTES],
                     const uint8_t client_pub[G4_PUB_BYTES], const uint8_t host_pub[G4_PUB_BYTES],
                     const uint8_t client_id[G4_ID_BYTES], const uint8_t host_id[G4_ID_BYTES]);
/* the 6-digit code both screens show while pairing (0..999999) */
uint32_t g4_pair_code(const uint8_t key[G4_KEY_BYTES]);
/* proof of the key for one connection. who: 'H' = host (Apple Silicon Mac),
   'C' = client (PowerPC Mac); different labels so one can't be replayed as the other */
void     g4_auth_tag(uint8_t tag[G4_TAG_BYTES], const uint8_t key[G4_KEY_BYTES], char who,
                     const uint8_t client_id[G4_ID_BYTES], const uint8_t host_id[G4_ID_BYTES],
                     const uint8_t client_nonce[G4_NONCE_BYTES], const uint8_t host_nonce[G4_NONCE_BYTES]);
/* constant-time compare: 1 = equal */
int      g4_equal(const uint8_t *a, const uint8_t *b, size_t n);
/* HMAC-SHA512 (exposed for tests) */
void     g4_hmac_sha512(uint8_t out[64], const uint8_t *key, size_t klen, const uint8_t *msg, size_t mlen);
/* lowercase hex; out needs 2n+1 bytes. g4_unhex: 0 = ok */
void     g4_hex(const uint8_t *in, size_t n, char *out);
int      g4_unhex(const char *in, uint8_t *out, size_t n);

#ifdef __cplusplus
}
#endif
#endif
