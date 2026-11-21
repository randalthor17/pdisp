/* Cryptographic primitives for the secure transport (docs/security.md), over Monocypher (third_party/monocypher):
 * ChaCha20-Poly1305 as in RFC 8439, X25519 (RFC 7748), BLAKE2b-512, HMAC-BLAKE2b and the Noise HKDF, and a system random source.
 * Nothing here keeps state; the Noise handshake and the transport are in noise.[ch] and secure.[ch]. */
#ifndef PDISP_CRYPTO_H
#define PDISP_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

#define PC_KEY_LEN 32
#define PC_TAG_LEN 16
#define PC_NONCE_LEN 12
#define PC_HASH_LEN 64 /* BLAKE2b-512 */
#define PC_DH_LEN 32

/* RFC 8439 AEAD: `ct` is the same length as `pt` (they may be the same buffer), the tag is separate. */
void pc_aead_seal(const uint8_t key[PC_KEY_LEN], const uint8_t nonce[PC_NONCE_LEN], const uint8_t *ad, size_t ad_len,
		  const uint8_t *pt, size_t len, uint8_t *ct, uint8_t tag[PC_TAG_LEN]);
/* Returns 0 and the plaintext, or -1 if the tag does not match (nothing is written to `pt` then). */
int pc_aead_open(const uint8_t key[PC_KEY_LEN], const uint8_t nonce[PC_NONCE_LEN], const uint8_t *ad, size_t ad_len,
		 const uint8_t *ct, size_t len, const uint8_t tag[PC_TAG_LEN], uint8_t *pt);

void pc_hash(uint8_t out[PC_HASH_LEN], const uint8_t *in, size_t len);
void pc_hash2(uint8_t out[PC_HASH_LEN], const uint8_t *a, size_t alen, const uint8_t *b, size_t blen);
/* HMAC with BLAKE2b-512 (block size 128), any key length. */
void pc_hmac(uint8_t out[PC_HASH_LEN], const uint8_t *key, size_t key_len, const uint8_t *a, size_t alen, const uint8_t *b, size_t blen);
/* The Noise HKDF: two or three 64-byte outputs from a chaining key and input key material. `out3` may be NULL. */
void pc_hkdf(const uint8_t ck[PC_HASH_LEN], const uint8_t *ikm, size_t ikm_len, uint8_t out1[PC_HASH_LEN], uint8_t out2[PC_HASH_LEN],
	     uint8_t out3[PC_HASH_LEN]);

void pc_x25519_public(uint8_t pub[PC_DH_LEN], const uint8_t secret[PC_DH_LEN]);
/* Returns 0, or -1 if the result is all zeros (a low-order public key: the peer is attacking or broken). */
int pc_x25519(uint8_t shared[PC_DH_LEN], const uint8_t secret[PC_DH_LEN], const uint8_t their_pub[PC_DH_LEN]);

/* Fills `buf` from the operating system; returns 0, or -1 if no randomness is available (never fall back to something weaker). */
int pc_random(uint8_t *buf, size_t len);
void pc_wipe(void *p, size_t len);
/* Constant-time comparison: 0 if equal. */
int pc_ct_compare(const uint8_t *a, const uint8_t *b, size_t len);

#endif
