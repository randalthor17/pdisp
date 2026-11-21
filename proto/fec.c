#include "fec.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* ---- GF(2^8) arithmetic */

static uint8_t gf_exp[512];
static uint8_t gf_log[256];
static uint8_t gf_mul_table[256][256]; /* gf_mul_table[a][b] = a * b */
static pthread_once_t gf_once = PTHREAD_ONCE_INIT;

static void gf_build(void)
{
	unsigned x = 1;
	for (int i = 0; i < 255; i++) {
		gf_exp[i] = (uint8_t)x;
		gf_log[x] = (uint8_t)i;
		x <<= 1;
		if (x & 0x100) x ^= 0x11d;
	}
	for (int i = 255; i < 512; i++) gf_exp[i] = gf_exp[i - 255];
	for (int a = 1; a < 256; a++)
		for (int b = 1; b < 256; b++) gf_mul_table[a][b] = gf_exp[gf_log[a] + gf_log[b]];
}

static void gf_init(void) { pthread_once(&gf_once, gf_build); }

static uint8_t gf_inv(uint8_t a) { return gf_exp[255 - gf_log[a]]; } /* a != 0 */

/* dst ^= coef * src, byte-wise */
static void mul_add(uint8_t *dst, const uint8_t *src, uint8_t coef, size_t len)
{
	if (coef == 0) return;
	if (coef == 1) {
		for (size_t i = 0; i < len; i++) dst[i] ^= src[i];
		return;
	}
	const uint8_t *row = gf_mul_table[coef];
	for (size_t i = 0; i < len; i++) dst[i] ^= row[src[i]];
}

/* The Cauchy coefficient of parity shard `s` for data shard `c` (s != c always holds since s >= k > c). */
static uint8_t cauchy(unsigned s, unsigned c) { return gf_inv((uint8_t)(s ^ c)); }

/* ---- public API */

int fec_encode_shard(unsigned k, unsigned index, const uint8_t *const data[], size_t len, uint8_t *out)
{
	if (k == 0 || index < k || index > FEC_MAX_INDEX || !data || !out) return -EINVAL;
	gf_init();
	memset(out, 0, len);
	for (unsigned c = 0; c < k; c++) mul_add(out, data[c], cauchy(index, c), len);
	return 0;
}

int fec_reconstruct(unsigned k, unsigned n, uint8_t *const shards[], const bool present[], size_t len)
{
	if (k == 0 || n < k || n > FEC_MAX_INDEX + 1u || !shards || !present) return -EINVAL;
	gf_init();

	unsigned missing[FEC_MAX_INDEX + 1], parity[FEC_MAX_INDEX + 1];
	unsigned e = 0, np = 0, have = 0;
	for (unsigned i = 0; i < n; i++) {
		if (present[i]) {
			have++;
			if (!shards[i]) return -EINVAL;
			if (i >= k) parity[np++] = i;
		} else if (i < k) {
			if (!shards[i]) return -EINVAL;
			missing[e++] = i;
		}
	}
	if (have < k) return -ENODATA;
	if (e == 0) return 0;
	if (np < e) return -ENODATA; /* cannot happen when have >= k, kept as a guard */

	/* syndromes: for each chosen parity row p, S_p = parity_p xor sum over present data c of C(p,c) * data_c
	 * which equals sum over missing c of C(p,c) * D_c */
	size_t synd_bytes = (size_t)e * len;
	uint8_t *synd = malloc(synd_bytes ? synd_bytes : 1);
	uint8_t *mat = malloc((size_t)e * e);
	uint8_t *inv = malloc((size_t)e * e);
	if (!synd || !mat || !inv) {
		free(synd); free(mat); free(inv);
		return -ENOMEM;
	}
	for (unsigned r = 0; r < e; r++) {
		uint8_t *s = synd + (size_t)r * len;
		memcpy(s, shards[parity[r]], len);
		for (unsigned c = 0; c < k; c++)
			if (present[c]) mul_add(s, shards[c], cauchy(parity[r], c), len);
		for (unsigned j = 0; j < e; j++) mat[r * e + j] = cauchy(parity[r], missing[j]);
	}

	/* invert the e x e Cauchy submatrix by Gauss-Jordan elimination */
	for (unsigned i = 0; i < e; i++)
		for (unsigned j = 0; j < e; j++) inv[i * e + j] = i == j;
	for (unsigned col = 0; col < e; col++) {
		unsigned piv = col;
		while (piv < e && mat[piv * e + col] == 0) piv++;
		if (piv == e) { free(synd); free(mat); free(inv); return -EINVAL; } /* singular: cannot happen for Cauchy */
		if (piv != col)
			for (unsigned j = 0; j < e; j++) {
				uint8_t t = mat[col * e + j]; mat[col * e + j] = mat[piv * e + j]; mat[piv * e + j] = t;
				t = inv[col * e + j]; inv[col * e + j] = inv[piv * e + j]; inv[piv * e + j] = t;
			}
		uint8_t d = gf_inv(mat[col * e + col]);
		for (unsigned j = 0; j < e; j++) {
			mat[col * e + j] = gf_mul_table[d][mat[col * e + j]];
			inv[col * e + j] = gf_mul_table[d][inv[col * e + j]];
		}
		for (unsigned r = 0; r < e; r++) {
			if (r == col) continue;
			uint8_t f = mat[r * e + col];
			if (!f) continue;
			for (unsigned j = 0; j < e; j++) {
				mat[r * e + j] ^= gf_mul_table[f][mat[col * e + j]];
				inv[r * e + j] ^= gf_mul_table[f][inv[col * e + j]];
			}
		}
	}

	/* D_missing[j] = sum over r of inv[j][r] * S_r */
	for (unsigned j = 0; j < e; j++) {
		uint8_t *d = shards[missing[j]];
		memset(d, 0, len);
		for (unsigned r = 0; r < e; r++) mul_add(d, synd + (size_t)r * len, inv[j * e + r], len);
	}
	free(synd);
	free(mat);
	free(inv);
	return 0;
}
