/* Known-answer tests for proto/crypto.c: RFC 8439 (ChaCha20-Poly1305), RFC 7748 (X25519), BLAKE2b, and HMAC/HKDF vectors from an
 * independent implementation (Python's hashlib and hmac), plus tamper and edge cases. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "crypto.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static size_t unhex(const char *s, uint8_t *out)
{
	size_t n = 0;
	for (; s[0] && s[1]; s += 2) { unsigned v; sscanf(s, "%2x", &v); out[n++] = (uint8_t)v; }
	return n;
}

static int hex_equal(const uint8_t *got, size_t n, const char *want)
{
	uint8_t w[512];
	return unhex(want, w) == n && memcmp(got, w, n) == 0;
}

static void test_aead(void)
{
	uint8_t key[32], nonce[12], ad[12], ct[200], tag[16], pt[200], back[200];
	for (int i = 0; i < 32; i++) key[i] = (uint8_t)(0x80 + i);
	unhex("070000004041424344454647", nonce);
	unhex("50515253c0c1c2c3c4c5c6c7", ad);
	const char *msg = "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.";
	size_t n = strlen(msg);
	memcpy(pt, msg, n);
	pc_aead_seal(key, nonce, ad, sizeof ad, pt, n, ct, tag);
	CHECK(hex_equal(ct, n, "d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d63dbea45e8ca9671282fafb69da92728b1a71de0a9e060b2905d6a5b67ecd3b3692ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585808b4831d7bc3ff4def08e4b7a9de576d26586cec64b6116"));
	CHECK(hex_equal(tag, 16, "1ae10b594f09e26a7e902ecbd0600691"));
	CHECK(pc_aead_open(key, nonce, ad, sizeof ad, ct, n, tag, back) == 0 && memcmp(back, msg, n) == 0);

	/* every kind of tampering is rejected, and nothing is decrypted into the output */
	memset(back, 0xaa, sizeof back);
	uint8_t bad[200];
	memcpy(bad, ct, n); bad[0] ^= 1;
	CHECK(pc_aead_open(key, nonce, ad, sizeof ad, bad, n, tag, back) == -1 && back[0] == 0xaa);
	memcpy(bad, ct, n); bad[n - 1] ^= 0x80;
	CHECK(pc_aead_open(key, nonce, ad, sizeof ad, bad, n, tag, back) == -1);
	uint8_t badtag[16]; memcpy(badtag, tag, 16); badtag[15] ^= 1;
	CHECK(pc_aead_open(key, nonce, ad, sizeof ad, ct, n, badtag, back) == -1);
	uint8_t badad[12]; memcpy(badad, ad, 12); badad[5] ^= 4;
	CHECK(pc_aead_open(key, nonce, badad, sizeof badad, ct, n, tag, back) == -1);
	CHECK(pc_aead_open(key, nonce, ad, sizeof ad - 1, ct, n, tag, back) == -1);
	uint8_t badnonce[12]; memcpy(badnonce, nonce, 12); badnonce[11] ^= 1;
	CHECK(pc_aead_open(key, badnonce, ad, sizeof ad, ct, n, tag, back) == -1);
	uint8_t badkey[32]; memcpy(badkey, key, 32); badkey[0] ^= 1;
	CHECK(pc_aead_open(badkey, nonce, ad, sizeof ad, ct, n, tag, back) == -1);

	/* lengths around the 16-byte padding boundaries, with the key and nonce 0..31 / 0..11 and ad "hdr" */
	static const struct { size_t n; const char *ct, *tag; } cases[] = {
		{ 0, "", "b893326ad7a430db7c9e28273736336f" },
		{ 1, "8a", "fc247c188ee187f672ee17ea02a95e81" },
		{ 15, "8af11918363188748cc176a3cf436b", "c1dd326bf4dab4629a96c2c850597ae3" },
		{ 16, "8af11918363188748cc176a3cf436b0f", "4fe1e630a37437a31ddd482c4d657709" },
		{ 17, "8af11918363188748cc176a3cf436b0fba", "fe525819cee34affedf11e30e0c9887d" },
		{ 64, "8af11918363188748cc176a3cf436b0fba0a336fdee2301d4d259605b20e73e00f6add0b1918bc8c2c105fe852aaca6f2ddec81c925ec51f992a40a737e12b6a", "61bcf02d9a634f7222bf3c9185de82e9" },
	};
	for (size_t c = 0; c < sizeof cases / sizeof cases[0]; c++) {
		uint8_t k[32], no[12], m[64], out[64], t[16];
		for (int i = 0; i < 32; i++) k[i] = (uint8_t)i;
		for (int i = 0; i < 12; i++) no[i] = (uint8_t)i;
		for (size_t i = 0; i < cases[c].n; i++) m[i] = (uint8_t)((i * 7 + 3) & 255);
		pc_aead_seal(k, no, (const uint8_t *)"hdr", 3, m, cases[c].n, out, t);
		CHECK(hex_equal(out, cases[c].n, cases[c].ct));
		CHECK(hex_equal(t, 16, cases[c].tag));
		/* in place */
		memcpy(out, m, cases[c].n);
		pc_aead_seal(k, no, (const uint8_t *)"hdr", 3, out, cases[c].n, out, t);
		CHECK(hex_equal(t, 16, cases[c].tag));
		CHECK(pc_aead_open(k, no, (const uint8_t *)"hdr", 3, out, cases[c].n, t, out) == 0 && memcmp(out, m, cases[c].n) == 0);
	}
}

static void test_hashes(void)
{
	uint8_t h[64], a[64], b[64], c[64], ck[64], ikm[32];
	pc_hash(h, (const uint8_t *)"abc", 3);
	CHECK(hex_equal(h, 64, "ba80a53f981c4d0d6a2797b69f12f6e94c212f14685ac4b74b12bb6fdbffa2d17d87c5392aab792dc252d5de4533cc9518d38aa8dbf1925ab92386edd4009923"));
	pc_hash2(a, (const uint8_t *)"a", 1, (const uint8_t *)"bc", 2);
	CHECK(memcmp(a, h, 64) == 0);

	pc_hmac(h, (const uint8_t *)"key", 3, (const uint8_t *)"The quick ", 10, (const uint8_t *)"brown fox", 9);
	CHECK(hex_equal(h, 64, "5c169e8a18362381efbc0f688fa1d48e99f2bd5885a19c29943e28b43d566f330555d6c37a277cd4cbb1deca561f377c02f0d5388c6e1efd6e97de2bd8b2d678"));
	uint8_t longkey[200];
	for (int i = 0; i < 200; i++) longkey[i] = (uint8_t)i;
	pc_hmac(h, longkey, 200, (const uint8_t *)"data", 4, NULL, 0);
	CHECK(hex_equal(h, 64, "dd83a68ea9b851310510d607d0313b685f724931a2a647eb5cd2c13189622293b8beb6035ca8b00005f251bb5ab9483e9c93bcbc10430e91fbbf9cd946c461c3"));

	for (int i = 0; i < 64; i++) ck[i] = (uint8_t)i;
	memcpy(ikm, "input key material", 18);
	pc_hkdf(ck, ikm, 18, a, b, c);
	CHECK(hex_equal(a, 64, "a94ec1ceaa5ac0280eafc97980003e4019176e4bf7c914c45fb0bbda0b22a9954d01f3697bbf32817ed0fccbf6dd1ebd24c0b05611051e63dc56e131af4f7c10"));
	CHECK(hex_equal(b, 64, "f9d83f6c5d34695529eb8e35c8a2ed78658092e7c3b38fdf0cea2a2096b6f6e542af644e298508f37da08860db285a8ad4b604c1f160f23356df6b28c183508d"));
	CHECK(hex_equal(c, 64, "bb6f14b645bac5dd79c98ce583e3357b43b431c22400c9b79d690c1d2fe2d391ab8372b0b08553b56e802dc6badace75398c1b8a973d0aab18d857d431c3a48f"));
	pc_hkdf(ck, NULL, 0, a, b, NULL);
	CHECK(hex_equal(a, 64, "03be0946afb62e33dcc2e8cb76dfad47663d284d81c0bde963f32a283dede221be2b9e3a86fa82934234d2a6ed331fb664a3e670252b9454da9ed2567b435e11"));
}

static void test_x25519_and_random(void)
{
	uint8_t s[32], u[32], out[32];
	unhex("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4", s);
	unhex("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c", u);
	CHECK(pc_x25519(out, s, u) == 0);
	CHECK(hex_equal(out, 32, "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552"));

	/* both sides of a key agreement get the same secret; a low-order public key is refused */
	uint8_t sa[32], sb[32], pa[32], pb[32], ka[32], kb[32];
	CHECK(pc_random(sa, 32) == 0 && pc_random(sb, 32) == 0);
	pc_x25519_public(pa, sa);
	pc_x25519_public(pb, sb);
	CHECK(pc_x25519(ka, sa, pb) == 0 && pc_x25519(kb, sb, pa) == 0 && memcmp(ka, kb, 32) == 0);
	uint8_t zero[32] = { 0 };
	CHECK(pc_x25519(out, sa, zero) == -1);

	uint8_t r1[32], r2[32];
	CHECK(pc_random(r1, 32) == 0 && pc_random(r2, 32) == 0 && memcmp(r1, r2, 32) != 0);
	CHECK(pc_ct_compare(r1, r1, 32) == 0 && pc_ct_compare(r1, r2, 32) != 0);
}

int main(void)
{
	test_aead();
	test_hashes();
	test_x25519_and_random();
	if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
	puts("crypto: all checks passed");
	return 0;
}
