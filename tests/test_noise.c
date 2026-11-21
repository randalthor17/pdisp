/* Noise XX handshake: byte-exact against an independent implementation (Python), random handshakes, and every way it must fail. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "noise.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static size_t unhex(const char *s, uint8_t *out)
{
	size_t n = 0;
	for (; s[0] && s[1]; s += 2) { unsigned v; sscanf(s, "%2x", &v); out[n++] = (uint8_t)v; }
	return n;
}
static int is_hex(const uint8_t *got, size_t n, const char *want)
{
	uint8_t w[512];
	return unhex(want, w) == n && memcmp(got, w, n) == 0;
}

static const uint8_t prologue[] = "pdisp noise test";

static void known_answer(void)
{
	uint8_t si[32], sr[32], ei[32], er[32];
	memset(si, 0x11, 32); memset(sr, 0x22, 32); memset(ei, 0x33, 32); memset(er, 0x44, 32);
	struct noise_hs I, R;
	noise_init(&I, NOISE_INITIATOR, si, prologue, sizeof prologue - 1);
	noise_init(&R, NOISE_RESPONDER, sr, prologue, sizeof prologue - 1);
	noise_set_ephemeral(&I, ei);
	noise_set_ephemeral(&R, er);
	uint8_t m[NOISE_MAX_MSG], pl[NOISE_MAX_PAYLOAD];

	int n = noise_write(&I, (const uint8_t *)"", 0, m, sizeof m);
	CHECK(n == 32 && is_hex(m, (size_t)n, "7b0d47d93427f8311160781c7c733fd89f88970aef490d8aa0ee19a4cb8a1b14"));
	CHECK(noise_read(&R, m, (size_t)n, pl, sizeof pl) == 0);

	n = noise_write(&R, (const uint8_t *)"PC: workstation", 15, m, sizeof m);
	CHECK(n > 0 && is_hex(m, (size_t)n, "ff2ee45601ec1b67310c7790404585ae697331eee1c1f8cf2419731c1fff3e6bb9bd2b1152eb07fbe44dda26d885bba10c98baffc5f360eec6c7009f8a73507ecc72bdab72970581298347f2ba304f07cc4350f32740519d3aaf59bc588c9e34ce916f243ea9d35665fb6feb19f46d"));
	int r = noise_read(&I, m, (size_t)n, pl, sizeof pl);
	CHECK(r == 15 && memcmp(pl, "PC: workstation", 15) == 0);

	n = noise_write(&I, (const uint8_t *)"Pixel 8a", 8, m, sizeof m);
	CHECK(n > 0 && is_hex(m, (size_t)n, "36a623ae1ee96b80978b36874a9edab0207e1cc76f8e83fda2c5a3c054b364919eaf6a597e455080d50e72b657cf6c3618edaf187f42c3f7bd54245c49ce9e632d2c8938fecec878"));
	r = noise_read(&R, m, (size_t)n, pl, sizeof pl);
	CHECK(r == 8 && memcmp(pl, "Pixel 8a", 8) == 0);

	CHECK(noise_done(&I) && noise_done(&R));
	struct noise_result ri, rr;
	CHECK(noise_finish(&I, &ri) == 0 && noise_finish(&R, &rr) == 0);
	CHECK(is_hex(ri.hash, 64, "fa19efcd4034e4f2c38018330fa439ba4e297e1504bd8640bf7d45e06c270c80eea6b3960668f1eeb56d07ca327189c16f535f6f77615b7ea9339e475eaaf9a4"));
	CHECK(memcmp(ri.hash, rr.hash, 64) == 0);
	CHECK(is_hex(ri.ctl_send, 32, "70399bc0fd822c4813e9b0405eb578e77fe49d2eebeccb998947de77b907494f") && memcmp(ri.ctl_send, rr.ctl_recv, 32) == 0);
	CHECK(is_hex(ri.ctl_recv, 32, "faa2cb0c2da5b5e0b66fb46b0dde13bfbd3447366505753b55383771eb42ad71") && memcmp(ri.ctl_recv, rr.ctl_send, 32) == 0);
	CHECK(is_hex(ri.udp_send, 32, "39c653e24e47b2dc08f5b79e5d9a51bc123330578c7c08802c820ef96491dfa0") && memcmp(ri.udp_send, rr.udp_recv, 32) == 0);
	CHECK(is_hex(ri.udp_recv, 32, "cb3a63f627f89bea31d9a75e7a58d4c4ba2946950705594f76809317f7e945d8") && memcmp(ri.udp_recv, rr.udp_send, 32) == 0);
	CHECK(noise_sas(ri.hash) == 263935 && noise_sas(rr.hash) == 263935);
	uint8_t pi[32], pr[32];
	pc_x25519_public(pi, si); pc_x25519_public(pr, sr);
	CHECK(memcmp(ri.peer_static, pr, 32) == 0 && memcmp(rr.peer_static, pi, 32) == 0);
	/* the key schedule keeps the four keys apart */
	CHECK(memcmp(ri.ctl_send, ri.udp_send, 32) != 0 && memcmp(ri.ctl_send, ri.ctl_recv, 32) != 0 && memcmp(ri.udp_send, ri.udp_recv, 32) != 0);
	CHECK(I.step == -1 && noise_finish(&I, &ri) == -EPROTO); /* the state is gone */
}

/* runs a whole handshake with random keys; returns the three messages for tampering tests when asked */
struct run { uint8_t m[3][NOISE_MAX_MSG]; int len[3]; uint8_t si[32], sr[32]; };

static void random_keys(struct run *x) { pc_random(x->si, 32); pc_random(x->sr, 32); }

static void random_handshake(void)
{
	struct run x;
	random_keys(&x);
	struct noise_hs I, R;
	noise_init(&I, NOISE_INITIATOR, x.si, prologue, sizeof prologue - 1);
	noise_init(&R, NOISE_RESPONDER, x.sr, prologue, sizeof prologue - 1);
	uint8_t pl[NOISE_MAX_PAYLOAD], big[NOISE_MAX_PAYLOAD];
	memset(big, 0x5a, sizeof big);
	x.len[0] = noise_write(&I, NULL, 0, x.m[0], NOISE_MAX_MSG);
	CHECK(noise_read(&R, x.m[0], (size_t)x.len[0], pl, sizeof pl) == 0);
	x.len[1] = noise_write(&R, big, sizeof big, x.m[1], NOISE_MAX_MSG); /* the largest payload */
	CHECK(noise_read(&I, x.m[1], (size_t)x.len[1], pl, sizeof pl) == (int)sizeof big && memcmp(pl, big, sizeof big) == 0);
	x.len[2] = noise_write(&I, big, 1, x.m[2], NOISE_MAX_MSG);
	CHECK(noise_read(&R, x.m[2], (size_t)x.len[2], pl, sizeof pl) == 1);
	struct noise_result a, b;
	CHECK(noise_finish(&I, &a) == 0 && noise_finish(&R, &b) == 0 && memcmp(a.hash, b.hash, 64) == 0);
	CHECK(memcmp(a.ctl_send, b.ctl_recv, 32) == 0 && memcmp(a.udp_send, b.udp_recv, 32) == 0);

	/* a second handshake with the same long-term keys has different transport keys (fresh ephemerals) */
	struct noise_hs I2, R2;
	noise_init(&I2, NOISE_INITIATOR, x.si, prologue, sizeof prologue - 1);
	noise_init(&R2, NOISE_RESPONDER, x.sr, prologue, sizeof prologue - 1);
	uint8_t m[NOISE_MAX_MSG];
	int n = noise_write(&I2, NULL, 0, m, sizeof m); noise_read(&R2, m, (size_t)n, pl, sizeof pl);
	n = noise_write(&R2, NULL, 0, m, sizeof m); noise_read(&I2, m, (size_t)n, pl, sizeof pl);
	n = noise_write(&I2, NULL, 0, m, sizeof m); noise_read(&R2, m, (size_t)n, pl, sizeof pl);
	struct noise_result c;
	CHECK(noise_finish(&I2, &c) == 0 && memcmp(a.ctl_send, c.ctl_send, 32) != 0 && memcmp(a.hash, c.hash, 64) != 0);
}

static void failures_and_order(void)
{
	struct run x;
	random_keys(&x);
	uint8_t pl[NOISE_MAX_PAYLOAD], m[NOISE_MAX_MSG];
	struct noise_hs I, R;

	/* different prologue: the responder's first answer cannot be read */
	noise_init(&I, NOISE_INITIATOR, x.si, prologue, sizeof prologue - 1);
	noise_init(&R, NOISE_RESPONDER, x.sr, (const uint8_t *)"other", 5);
	int n = noise_write(&I, NULL, 0, m, sizeof m);
	CHECK(noise_read(&R, m, (size_t)n, pl, sizeof pl) == 0); /* msg1 carries no secret, so it still parses */
	n = noise_write(&R, (const uint8_t *)"x", 1, m, sizeof m);
	CHECK(noise_read(&I, m, (size_t)n, pl, sizeof pl) == -EBADMSG);
	CHECK(noise_write(&I, NULL, 0, m, sizeof m) < 0); /* dead after a failure */

	/* flipping any single byte of msg2 or msg3 is detected */
	for (int which = 1; which <= 2; which++) {
		int len0 = 0, len1 = 0, len2 = 0;
		uint8_t m0[NOISE_MAX_MSG], m1[NOISE_MAX_MSG], m2[NOISE_MAX_MSG];
		struct noise_hs i, r;
		noise_init(&i, NOISE_INITIATOR, x.si, prologue, sizeof prologue - 1);
		noise_init(&r, NOISE_RESPONDER, x.sr, prologue, sizeof prologue - 1);
		len0 = noise_write(&i, NULL, 0, m0, sizeof m0);
		noise_read(&r, m0, (size_t)len0, pl, sizeof pl);
		len1 = noise_write(&r, (const uint8_t *)"name", 4, m1, sizeof m1);
		struct noise_hs i_after = i;
		noise_read(&i, m1, (size_t)len1, pl, sizeof pl);
		len2 = noise_write(&i, (const uint8_t *)"phone", 5, m2, sizeof m2);
		struct noise_hs r_after = r;
		int bad = 0, total = 0;
		const uint8_t *msg = which == 1 ? m1 : m2;
		int len = which == 1 ? len1 : len2;
		for (int pos = 0; pos < len; pos++) {
			uint8_t t[NOISE_MAX_MSG];
			memcpy(t, msg, (size_t)len);
			t[pos] ^= 0x01;
			struct noise_hs victim = which == 1 ? i_after : r_after;
			total++;
			if (noise_read(&victim, t, (size_t)len, pl, sizeof pl) < 0) bad++;
		}
		CHECK(bad == total);
		/* truncated */
		struct noise_hs v1 = which == 1 ? i_after : r_after, v2 = v1, v3 = v1;
		CHECK(noise_read(&v1, msg, (size_t)len - 1, pl, sizeof pl) < 0);
		CHECK(noise_read(&v2, msg, 20, pl, sizeof pl) < 0);
		CHECK(noise_read(&v3, msg, 0, pl, sizeof pl) < 0);
	}

	/* a low-order ephemeral key from the initiator makes the responder's reply fail rather than leak a known key */
	noise_init(&I, NOISE_INITIATOR, x.si, prologue, sizeof prologue - 1);
	noise_init(&R, NOISE_RESPONDER, x.sr, prologue, sizeof prologue - 1);
	uint8_t zero_e[32] = { 0 };
	n = noise_write(&I, NULL, 0, m, sizeof m);
	memcpy(m, zero_e, 32);
	CHECK(noise_read(&R, m, (size_t)n, pl, sizeof pl) == 0);
	CHECK(noise_write(&R, NULL, 0, m, sizeof m) == -EBADMSG);

	/* wrong order, wrong role, early finish, oversize payload, small buffers */
	noise_init(&I, NOISE_INITIATOR, x.si, prologue, sizeof prologue - 1);
	noise_init(&R, NOISE_RESPONDER, x.sr, prologue, sizeof prologue - 1);
	struct noise_result res;
	CHECK(noise_read(&I, m, 32, pl, sizeof pl) == -EPROTO);   /* the initiator speaks first */
	CHECK(noise_write(&R, NULL, 0, m, sizeof m) == -EPROTO);  /* the responder listens first */
	CHECK(noise_finish(&I, &res) == -EPROTO);
	uint8_t big[NOISE_MAX_PAYLOAD + 1] = { 0 };
	CHECK(noise_write(&I, big, sizeof big, m, sizeof m) == -EMSGSIZE);
	CHECK(noise_write(&I, NULL, 0, m, 10) == -ENOSPC);
	n = noise_write(&I, NULL, 0, m, sizeof m);
	CHECK(noise_write(&I, NULL, 0, m, sizeof m) == -EPROTO);  /* not twice in a row */
	CHECK(noise_read(&R, m, (size_t)n, pl, sizeof pl) == 0);
	CHECK(noise_read(&R, m, (size_t)n, pl, sizeof pl) == -EPROTO); /* a replayed first message */
}

static void unknown_peer_is_visible(void)
{
	/* the handshake succeeds with any static key: deciding whether to trust it is the caller's job, so it must be readable */
	uint8_t sa[32], sb[32], pa[32];
	pc_random(sa, 32); pc_random(sb, 32);
	pc_x25519_public(pa, sa);
	struct noise_hs I, R;
	noise_init(&I, NOISE_INITIATOR, sa, prologue, sizeof prologue - 1);
	noise_init(&R, NOISE_RESPONDER, sb, prologue, sizeof prologue - 1);
	uint8_t m[NOISE_MAX_MSG], pl[NOISE_MAX_PAYLOAD];
	int n = noise_write(&I, NULL, 0, m, sizeof m); noise_read(&R, m, (size_t)n, pl, sizeof pl);
	n = noise_write(&R, NULL, 0, m, sizeof m); noise_read(&I, m, (size_t)n, pl, sizeof pl);
	n = noise_write(&I, NULL, 0, m, sizeof m); noise_read(&R, m, (size_t)n, pl, sizeof pl);
	struct noise_result rr;
	CHECK(noise_finish(&R, &rr) == 0 && memcmp(rr.peer_static, pa, 32) == 0);
}

int main(void)
{
	known_answer();
	random_handshake();
	failures_and_order();
	unknown_peer_is_visible();
	if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
	puts("noise: all checks passed");
	return 0;
}
