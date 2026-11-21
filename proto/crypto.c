#include "crypto.h"

#include <errno.h>
#include <string.h>
#include <sys/random.h>

#include "monocypher.h"

static void le64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }

/* poly1305 over the RFC 8439 construction: ad, padding, ciphertext, padding, lengths */
static void aead_mac(uint8_t tag[16], const uint8_t polykey[32], const uint8_t *ad, size_t ad_len, const uint8_t *ct, size_t len)
{
	static const uint8_t zeros[16];
	crypto_poly1305_ctx c;
	crypto_poly1305_init(&c, polykey);
	crypto_poly1305_update(&c, ad, ad_len);
	if (ad_len % 16) crypto_poly1305_update(&c, zeros, 16 - ad_len % 16);
	crypto_poly1305_update(&c, ct, len);
	if (len % 16) crypto_poly1305_update(&c, zeros, 16 - len % 16);
	uint8_t lens[16];
	le64(lens, ad_len);
	le64(lens + 8, len);
	crypto_poly1305_update(&c, lens, 16);
	crypto_poly1305_final(&c, tag);
}

static void poly_key(uint8_t out[32], const uint8_t key[32], const uint8_t nonce[12])
{
	static const uint8_t zeros[32];
	crypto_chacha20_ietf(out, zeros, 32, key, nonce, 0); /* the first keystream block, counter 0 */
}

void pc_aead_seal(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *ad, size_t ad_len, const uint8_t *pt, size_t len,
		  uint8_t *ct, uint8_t tag[16])
{
	uint8_t pk[32];
	poly_key(pk, key, nonce);
	crypto_chacha20_ietf(ct, pt, len, key, nonce, 1);
	aead_mac(tag, pk, ad, ad_len, ct, len);
	crypto_wipe(pk, sizeof pk);
}

int pc_aead_open(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *ad, size_t ad_len, const uint8_t *ct, size_t len,
		 const uint8_t tag[16], uint8_t *pt)
{
	uint8_t pk[32], want[16];
	poly_key(pk, key, nonce);
	aead_mac(want, pk, ad, ad_len, ct, len);
	crypto_wipe(pk, sizeof pk);
	if (crypto_verify16(want, tag) != 0) return -1;
	crypto_chacha20_ietf(pt, ct, len, key, nonce, 1);
	return 0;
}

void pc_hash(uint8_t out[64], const uint8_t *in, size_t len) { crypto_blake2b(out, 64, in, len); }

void pc_hash2(uint8_t out[64], const uint8_t *a, size_t alen, const uint8_t *b, size_t blen)
{
	crypto_blake2b_ctx c;
	crypto_blake2b_init(&c, 64);
	crypto_blake2b_update(&c, a, alen);
	if (blen) crypto_blake2b_update(&c, b, blen);
	crypto_blake2b_final(&c, out);
}

void pc_hmac(uint8_t out[64], const uint8_t *key, size_t key_len, const uint8_t *a, size_t alen, const uint8_t *b, size_t blen)
{
	uint8_t k[128], pad[128], inner[64];
	memset(k, 0, sizeof k);
	if (key_len > 128) crypto_blake2b(k, 64, key, key_len); else if (key_len) memcpy(k, key, key_len);
	crypto_blake2b_ctx c;
	for (int i = 0; i < 128; i++) pad[i] = k[i] ^ 0x36;
	crypto_blake2b_init(&c, 64);
	crypto_blake2b_update(&c, pad, 128);
	if (alen) crypto_blake2b_update(&c, a, alen);
	if (blen) crypto_blake2b_update(&c, b, blen);
	crypto_blake2b_final(&c, inner);
	for (int i = 0; i < 128; i++) pad[i] = k[i] ^ 0x5c;
	crypto_blake2b_init(&c, 64);
	crypto_blake2b_update(&c, pad, 128);
	crypto_blake2b_update(&c, inner, 64);
	crypto_blake2b_final(&c, out);
	crypto_wipe(k, sizeof k);
	crypto_wipe(pad, sizeof pad);
	crypto_wipe(inner, sizeof inner);
}

void pc_hkdf(const uint8_t ck[64], const uint8_t *ikm, size_t ikm_len, uint8_t out1[64], uint8_t out2[64], uint8_t out3[64])
{
	uint8_t temp[64], b;
	pc_hmac(temp, ck, 64, ikm, ikm_len, NULL, 0);
	b = 1;
	pc_hmac(out1, temp, 64, &b, 1, NULL, 0);
	b = 2;
	pc_hmac(out2, temp, 64, out1, 64, &b, 1);
	if (out3) {
		b = 3;
		pc_hmac(out3, temp, 64, out2, 64, &b, 1);
	}
	crypto_wipe(temp, sizeof temp);
}

void pc_x25519_public(uint8_t pub[32], const uint8_t secret[32]) { crypto_x25519_public_key(pub, secret); }

int pc_x25519(uint8_t shared[32], const uint8_t secret[32], const uint8_t their_pub[32])
{
	crypto_x25519(shared, secret, their_pub);
	static const uint8_t zero[32];
	return pc_ct_compare(shared, zero, 32) == 0 ? -1 : 0;
}

int pc_random(uint8_t *buf, size_t len)
{
	while (len) {
		ssize_t n = getrandom(buf, len > 256 ? 256 : len, 0);
		if (n < 0) {
			if (errno == EINTR) continue;
			return -1;
		}
		buf += n;
		len -= (size_t)n;
	}
	return 0;
}

void pc_wipe(void *p, size_t len) { crypto_wipe(p, len); }

int pc_ct_compare(const uint8_t *a, const uint8_t *b, size_t len)
{
	uint8_t d = 0;
	for (size_t i = 0; i < len; i++) d |= a[i] ^ b[i];
	return d != 0;
}
