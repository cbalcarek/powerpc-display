/* Minimal declarations for the vendored TweetNaCl functions PowerPC Display uses. */
#ifndef G4NACL_H
#define G4NACL_H
int crypto_scalarmult_curve25519_tweet(unsigned char *q, const unsigned char *n, const unsigned char *p);
int crypto_scalarmult_curve25519_tweet_base(unsigned char *q, const unsigned char *n);
int crypto_hash_sha512_tweet(unsigned char *out, const unsigned char *m, unsigned long long n);
#define crypto_scalarmult      crypto_scalarmult_curve25519_tweet
#define crypto_scalarmult_base crypto_scalarmult_curve25519_tweet_base
#define crypto_hash            crypto_hash_sha512_tweet
/* provided by g4auth.c (reads /dev/urandom) */
void randombytes(unsigned char *x, unsigned long long n);
#endif
