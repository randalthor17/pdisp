/* Unit tests for the HEVC parameter set and slice header writers. The stream-level correctness (decodes, VUI,
 * profile) is checked by the M3 integration test with ffprobe; this covers the framing rules. */
#include "hevc_headers.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond)                                                                                  \
	do {                                                                                         \
		if (!(cond)) {                                                                       \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);     \
			failures++;                                                                  \
		}                                                                                    \
	} while (0)

static struct hevc_params params(void)
{
	return (struct hevc_params){ .width = 2304, .height = 1440, .fps = 60, .level_idc = 150, .init_qp = 26,
				     .colour_primaries = 1, .transfer_characteristics = 13, .matrix_coefficients = 1 };
}

struct nal { const uint8_t *data; size_t len; unsigned type; };

/* split an Annex B buffer on 4-byte start codes (the writer only uses those) */
static int split(const uint8_t *buf, size_t len, struct nal *out, int max)
{
	int n = 0;
	for (size_t i = 0; i + 4 <= len; i++) {
		if (buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 0 && buf[i + 3] == 1 && n < max) {
			out[n].data = buf + i + 4;
			out[n].type = (buf[i + 4] >> 1) & 0x3f;
			if (n > 0) out[n - 1].len = (size_t)(buf + i - out[n - 1].data);
			n++;
			i += 3;
		}
	}
	if (n > 0) out[n - 1].len = (size_t)(buf + len - out[n - 1].data);
	return n;
}

/* payload after the 2-byte NAL header must never contain 00 00 00, 00 00 01 or 00 00 02 */
static int no_start_code_emulation(const struct nal *n)
{
	for (size_t i = 2; i + 2 < n->len; i++)
		if (n->data[i] == 0 && n->data[i + 1] == 0 && n->data[i + 2] <= 2) return 0;
	return n->data[n->len - 1] != 0;
}

struct br { const uint8_t *d; size_t len, pos; };
static unsigned rbit(struct br *b) { unsigned v = (b->d[b->pos / 8] >> (7 - b->pos % 8)) & 1; b->pos++; return v; }
static unsigned rbits(struct br *b, int n) { unsigned v = 0; while (n--) v = v << 1 | rbit(b); return v; }
static unsigned rue(struct br *b)
{
	int z = 0;
	while (!rbit(b)) z++;
	return (1u << z) - 1 + (z ? rbits(b, z) : 0);
}

/* undo emulation prevention */
static size_t unescape(const struct nal *n, uint8_t *out)
{
	size_t o = 0;
	unsigned zeros = 0;
	for (size_t i = 2; i < n->len; i++) {
		if (zeros >= 2 && n->data[i] == 3) { zeros = 0; continue; }
		out[o++] = n->data[i];
		zeros = n->data[i] == 0 ? zeros + 1 : 0;
	}
	return o;
}

static void test_parameter_sets(void)
{
	struct hevc_params p = params();
	uint8_t buf[512];
	size_t len = hevc_write_parameter_sets(&p, buf, sizeof buf);
	CHECK(len > 40 && len < 200);

	struct nal nals[4];
	int n = split(buf, len, nals, 4);
	CHECK(n == 3);
	CHECK(nals[0].type == 32 && nals[1].type == 33 && nals[2].type == 34);
	for (int i = 0; i < n; i++) {
		CHECK(nals[i].data[1] == 1); /* layer 0, temporal id 0 */
		CHECK(no_start_code_emulation(&nals[i]));
	}

	/* decode the SPS far enough to read the picture size back */
	uint8_t rbsp[256];
	size_t rlen = unescape(&nals[1], rbsp);
	struct br b = { rbsp, rlen, 0 };
	rbits(&b, 4 + 3 + 1); /* vps id, max sub layers, nesting */
	rbits(&b, 32); rbits(&b, 32); rbits(&b, 32); /* profile_tier_level: 96 bits */
	CHECK(rue(&b) == 0); /* sps id */
	CHECK(rue(&b) == 1); /* 4:2:0 */
	CHECK(rue(&b) == 2304);
	CHECK(rue(&b) == 1440);

	/* sizes that are not a multiple of 8 and tiny buffers are refused */
	p.width = 2300;
	CHECK(hevc_write_parameter_sets(&p, buf, sizeof buf) == 0);
	p = params();
	CHECK(hevc_write_parameter_sets(&p, buf, 40) == 0);

	/* a size whose bytes force emulation prevention still frames correctly */
	p.width = 2048; p.height = 2048;
	len = hevc_write_parameter_sets(&p, buf, sizeof buf);
	CHECK(len > 0);
	n = split(buf, len, nals, 4);
	CHECK(n == 3);
	for (int i = 0; i < n; i++) CHECK(no_start_code_emulation(&nals[i]));
}

static void test_slice_headers(void)
{
	struct hevc_params p = params();
	uint8_t buf[64];
	struct nal nals[2];

	size_t len = hevc_write_slice_header(&p, true, 0, buf, sizeof buf);
	CHECK(len > 5 && len < 16);
	CHECK(split(buf, len, nals, 2) == 1 && nals[0].type == 19); /* IDR_W_RADL */
	CHECK(no_start_code_emulation(&nals[0]));

	for (unsigned poc = 1; poc < 5000; poc += 37) {
		len = hevc_write_slice_header(&p, false, poc, buf, sizeof buf);
		CHECK(len > 5 && len < 24);
		CHECK(split(buf, len, nals, 2) == 1 && nals[0].type == 1); /* TRAIL_R */
		CHECK(no_start_code_emulation(&nals[0]));
	}
	CHECK(hevc_write_slice_header(&p, false, 1, buf, 6) == 0);
}

int main(void)
{
	test_parameter_sets();
	test_slice_headers();
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	puts("hevc_headers: all checks passed");
	return 0;
}
