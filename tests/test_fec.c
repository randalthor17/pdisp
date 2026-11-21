/* Tests for the Reed-Solomon erasure code (proto/fec.c). */
#include "fec.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int failures;
#define CHECK(cond)                                                                                  \
	do {                                                                                         \
		if (!(cond)) {                                                                       \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);     \
			failures++;                                                                  \
		}                                                                                    \
	} while (0)

static uint64_t rng = 0x2545f4914f6cdd1dull;
static uint32_t rnd(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 7;
	rng ^= rng << 17;
	return (uint32_t)(rng >> 16);
}

/* ---- hand-computed vectors. In GF(2^8) with polynomial 0x11d: inv(2) = 0x8e (2 * 0x8e = 0x11c = 0x11d xor 1) and
 * inv(3) = 0xf4 (3 * 0xf4 = 0x1e8 xor 0x11d xor 0xf4 = 1). With k = 2 the parity coefficients are
 * shard 2: [inv(2 xor 0), inv(2 xor 1)] = [inv(2), inv(3)]; shard 3: [inv(3 xor 0), inv(3 xor 1)] = [inv(3), inv(2)]. */
static void test_vectors(void)
{
	uint8_t d0 = 0x01, d1 = 0x00, out;
	const uint8_t *data[2] = { &d0, &d1 };
	CHECK(fec_encode_shard(2, 2, data, 1, &out) == 0 && out == 0x8e);
	CHECK(fec_encode_shard(2, 3, data, 1, &out) == 0 && out == 0xf4);
	d0 = 0x01; d1 = 0x01;
	CHECK(fec_encode_shard(2, 2, data, 1, &out) == 0 && out == (0x8e ^ 0xf4)); /* 0x7a */
	CHECK(fec_encode_shard(2, 3, data, 1, &out) == 0 && out == (0xf4 ^ 0x8e));

	/* with k = 1 a parity shard is the data scaled by a constant; losing the data shard must be recoverable from it */
	uint8_t one = 0x05;
	const uint8_t *data1[1] = { &one };
	uint8_t par;
	CHECK(fec_encode_shard(1, 1, data1, 1, &par) == 0);
	uint8_t recovered = 0;
	uint8_t *sh[2] = { &recovered, &par };
	bool present[2] = { false, true };
	CHECK(fec_reconstruct(1, 2, sh, present, 1) == 0 && recovered == 0x05);
}

static void fill_random(uint8_t *p, size_t n)
{
	for (size_t i = 0; i < n; i++) p[i] = (uint8_t)rnd();
}

/* Encode a block, drop some shards (any k survive), reconstruct, compare with the original data. */
static void roundtrip(unsigned k, unsigned m, size_t len, int trials)
{
	unsigned n = k + m;
	uint8_t *orig = malloc((size_t)k * len + 1), *par = malloc((size_t)m * len + 1);
	uint8_t *work = malloc((size_t)n * len + 1);
	const uint8_t *data[FEC_MAX_INDEX + 1];
	uint8_t *shards[FEC_MAX_INDEX + 1];
	bool present[FEC_MAX_INDEX + 1];

	fill_random(orig, (size_t)k * len);
	for (unsigned c = 0; c < k; c++) data[c] = orig + (size_t)c * len;
	for (unsigned j = 0; j < m; j++) CHECK(fec_encode_shard(k, k + j, data, len, par + (size_t)j * len) == 0);

	for (int t = 0; t < trials; t++) {
		/* choose how many shards to lose (0..m) and which */
		unsigned lose = rnd() % (m + 1);
		if (t == 0) lose = m;                    /* always include the worst case */
		memset(present, 1, sizeof present);
		for (unsigned l = 0; l < lose;) {
			unsigned i = rnd() % n;
			if (present[i]) { present[i] = false; l++; }
		}
		if (t == 1 && m >= k) { /* lose every data shard, keep all parity */
			memset(present, 1, sizeof present);
			for (unsigned i = 0; i < k; i++) present[i] = false;
		}

		unsigned have = 0;
		for (unsigned i = 0; i < n; i++) have += present[i];
		for (unsigned i = 0; i < n; i++) {
			shards[i] = work + (size_t)i * len;
			if (i < k) {
				if (present[i]) memcpy(shards[i], orig + (size_t)i * len, len);
				else memset(shards[i], 0xa5, len); /* garbage that must be overwritten */
			} else {
				memcpy(shards[i], par + (size_t)(i - k) * len, len);
			}
		}
		int rc = fec_reconstruct(k, n, shards, present, len);
		if (have < k) { CHECK(rc == -ENODATA); continue; }
		CHECK(rc == 0);
		for (unsigned c = 0; c < k; c++)
			if (memcmp(shards[c], orig + (size_t)c * len, len) != 0) {
				fprintf(stderr, "k=%u m=%u len=%zu trial %d: data shard %u wrong\n", k, m, len, t, c);
				failures++;
				break;
			}
	}
	free(orig); free(par); free(work);
}

static void test_roundtrips(void)
{
	static const struct { unsigned k, m; } cfg[] = { { 1, 1 }, { 1, 6 }, { 2, 1 }, { 3, 2 }, { 4, 4 }, { 10, 3 }, { 10, 10 },
							 { 50, 10 }, { 84, 17 }, { 128, 127 }, { 200, 55 }, { 100, 155 }, { 254, 1 } };
	for (size_t i = 0; i < sizeof cfg / sizeof *cfg; i++) {
		roundtrip(cfg[i].k, cfg[i].m, 1200, 60);
		roundtrip(cfg[i].k, cfg[i].m, 1, 30);
	}
	roundtrip(8, 4, 0, 10); /* empty shards are valid and trivial */
}

/* Repair shards: parity indices need not be contiguous, and a block can be repaired later with indices well beyond the
 * initial parity count. */
static void test_sparse_parity(void)
{
	const unsigned k = 20;
	const size_t len = 257;
	uint8_t orig[20 * 257], par[FEC_MAX_INDEX + 1][257], work[FEC_MAX_INDEX + 1][257];
	const uint8_t *data[20];
	fill_random(orig, sizeof orig);
	for (unsigned c = 0; c < k; c++) data[c] = orig + (size_t)c * len;
	for (unsigned s = k; s <= FEC_MAX_INDEX; s++) CHECK(fec_encode_shard(k, s, data, len, par[s]) == 0);

	for (int t = 0; t < 200; t++) {
		unsigned lose = 1 + rnd() % 12;
		uint8_t *shards[FEC_MAX_INDEX + 1];
		bool present[FEC_MAX_INDEX + 1];
		memset(present, 0, sizeof present);
		for (unsigned i = 0; i < k; i++) {
			present[i] = true;
			shards[i] = work[i];
			memcpy(work[i], orig + (size_t)i * len, len);
		}
		for (unsigned l = 0; l < lose;) { /* lose data shards */
			unsigned i = rnd() % k;
			if (present[i]) { present[i] = false; memset(work[i], 0, len); l++; }
		}
		for (unsigned l = 0; l < lose;) { /* receive `lose` random parity shards from anywhere in k..254 */
			unsigned s = k + rnd() % (FEC_MAX_INDEX + 1 - k);
			if (!present[s]) { present[s] = true; shards[s] = par[s]; l++; }
		}
		CHECK(fec_reconstruct(k, FEC_MAX_INDEX + 1, shards, present, len) == 0);
		CHECK(memcmp(work, orig, sizeof orig) == 0);
	}
}

static void test_errors(void)
{
	uint8_t a[4] = { 1, 2, 3, 4 }, b[4] = { 5, 6, 7, 8 }, p[4];
	const uint8_t *data[2] = { a, b };
	CHECK(fec_encode_shard(2, 1, data, 4, p) == -EINVAL);                 /* index inside the data range */
	CHECK(fec_encode_shard(2, FEC_MAX_INDEX + 1, data, 4, p) == -EINVAL); /* past the last index */
	CHECK(fec_encode_shard(0, 0, data, 4, p) == -EINVAL);
	CHECK(fec_encode_shard(2, 2, NULL, 4, p) == -EINVAL);

	uint8_t x[4], y[4], z[4];
	uint8_t *sh[3] = { x, y, z };
	bool present[3] = { false, false, true };
	CHECK(fec_reconstruct(2, 3, sh, present, 4) == -ENODATA);              /* only 1 of 2 needed shards */
	bool all[3] = { true, true, true };
	CHECK(fec_reconstruct(2, 3, sh, all, 4) == 0);                         /* nothing missing */
	CHECK(fec_reconstruct(0, 3, sh, all, 4) == -EINVAL);
	CHECK(fec_reconstruct(4, 3, sh, all, 4) == -EINVAL);                   /* n < k */
	uint8_t *nul[3] = { x, NULL, z };
	bool miss1[3] = { true, false, true };
	CHECK(fec_reconstruct(2, 3, nul, miss1, 4) == -EINVAL);                /* missing data shard without a buffer */
}

static double now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

/* not a pass/fail check beyond a sanity bound: report the cost of a large IDR (84 data + 17 parity shards) */
static void test_cost(void)
{
	const unsigned k = 84, m = 17;
	const size_t len = 1200;
	static uint8_t orig[84 * 1200], par[17 * 1200], work[101 * 1200];
	const uint8_t *data[84];
	fill_random(orig, sizeof orig);
	for (unsigned c = 0; c < k; c++) data[c] = orig + (size_t)c * len;

	double t0 = now_ms();
	for (unsigned j = 0; j < m; j++) fec_encode_shard(k, k + j, data, len, par + (size_t)j * len);
	double t1 = now_ms();

	uint8_t *shards[101];
	bool present[101];
	for (unsigned i = 0; i < k + m; i++) {
		shards[i] = work + (size_t)i * len;
		present[i] = true;
		memcpy(shards[i], i < k ? orig + (size_t)i * len : par + (size_t)(i - k) * len, len);
	}
	for (unsigned l = 0; l < 10; l++) present[l * 8] = false; /* lose 10 data shards */
	double t2 = now_ms();
	CHECK(fec_reconstruct(k, k + m, shards, present, len) == 0);
	double t3 = now_ms();
	CHECK(memcmp(work, orig, sizeof orig) == 0);
	printf("fec cost: encode 17 parity shards of an 84-shard frame (100 KB): %.2f ms; reconstruct 10 lost shards: %.2f ms\n", t1 - t0, t3 - t2);
	CHECK(t1 - t0 < 50.0 && t3 - t2 < 50.0);
}

int main(void)
{
	test_vectors();
	test_roundtrips();
	test_sparse_parity();
	test_errors();
	test_cost();
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	puts("fec: all checks passed");
	return 0;
}
