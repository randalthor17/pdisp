/* Unit tests for the H.264 parameter set and slice header writers. The stream-level correctness (it decodes, loss recovery is bit-exact)
 * is checked by tests/loss_recovery_check.sh with the real encoder; this covers the framing rules and reads the headers back. */
#include "h264_headers.h"

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

static struct h264_params params(void)
{
	return (struct h264_params){ .width = 2304, .height = 1440, .fps = 90, .level_idc = 52, .max_num_ref_frames = 6, .init_qp = 26,
				     .colour_primaries = 1, .transfer_characteristics = 13, .matrix_coefficients = 1 };
}

struct nal { const uint8_t *data; size_t len; unsigned type, ref_idc; };

/* split an Annex B buffer on 4-byte start codes (the writer only uses those) */
static int split(const uint8_t *buf, size_t len, struct nal *out, int max)
{
	int n = 0;
	for (size_t i = 0; i + 4 <= len; i++) {
		if (buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 0 && buf[i + 3] == 1 && n < max) {
			out[n].data = buf + i + 4;
			out[n].type = buf[i + 4] & 0x1f;
			out[n].ref_idc = (buf[i + 4] >> 5) & 3;
			if (n > 0) out[n - 1].len = (size_t)(buf + i - out[n - 1].data);
			n++;
			i += 3;
		}
	}
	if (n > 0) out[n - 1].len = (size_t)(buf + len - out[n - 1].data);
	return n;
}

/* the payload after the 1-byte NAL header must never contain 00 00 00, 00 00 01 or 00 00 02, and must not end in 00 */
static int no_start_code_emulation(const struct nal *n)
{
	for (size_t i = 1; i + 2 < n->len; i++)
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

/* undo emulation prevention, skipping the 1-byte NAL header */
static size_t unescape(const struct nal *n, uint8_t *out)
{
	size_t o = 0;
	unsigned zeros = 0;
	for (size_t i = 1; i < n->len; i++) {
		if (zeros >= 2 && n->data[i] == 3) { zeros = 0; continue; }
		out[o++] = n->data[i];
		zeros = n->data[i] == 0 ? zeros + 1 : 0;
	}
	return o;
}

static void test_level(void)
{
	CHECK(h264_level_idc(1280, 800, 60) == 40);
	CHECK(h264_level_idc(1920, 1088, 60) == 42);
	CHECK(h264_level_idc(2304, 1440, 60) == 51);
	CHECK(h264_level_idc(2304, 1440, 90) == 52);
	CHECK(h264_level_idc(1440, 2304, 120) == 52); /* the macroblock rate is the same either way up */
	CHECK(h264_level_idc(7680, 4320, 60) >= 60);
}

static void test_parameter_sets(void)
{
	struct h264_params p = params();
	uint8_t buf[512];
	size_t len = h264_write_parameter_sets(&p, buf, sizeof buf);
	CHECK(len > 30 && len < 150);

	struct nal nals[4];
	int n = split(buf, len, nals, 4);
	CHECK(n == 2);
	CHECK(nals[0].type == 7 && nals[1].type == 8);
	CHECK(nals[0].ref_idc == 3 && nals[1].ref_idc == 3);
	for (int i = 0; i < n; i++) CHECK(no_start_code_emulation(&nals[i]));

	/* read the SPS back */
	uint8_t rbsp[256];
	size_t rlen = unescape(&nals[0], rbsp);
	struct br b = { rbsp, rlen, 0 };
	CHECK(rbits(&b, 8) == 100);        /* High */
	rbits(&b, 8);                      /* constraint flags */
	CHECK(rbits(&b, 8) == 52);         /* level */
	CHECK(rue(&b) == 0);               /* sps id */
	CHECK(rue(&b) == 1);               /* 4:2:0 */
	CHECK(rue(&b) == 0 && rue(&b) == 0); /* 8 bit */
	rbits(&b, 2);
	CHECK(rue(&b) == H264_LOG2_MAX_FRAME_NUM - 4);
	CHECK(rue(&b) == 2);               /* POC type 2 */
	CHECK(rue(&b) == 6);               /* reference frames */
	CHECK(rbit(&b) == 1);              /* gaps in frame_num allowed */
	CHECK(rue(&b) == 2304 / 16 - 1);
	CHECK(rue(&b) == 1440 / 16 - 1);
	CHECK(rbit(&b) == 1);              /* frame_mbs_only */
	rbit(&b);                          /* direct 8x8 */
	CHECK(rbit(&b) == 0);              /* no cropping: both sizes are multiples of 16 */

	/* a size that is not a multiple of 16 is cropped: 2312 x 1448 codes 145 x 91 macroblocks and crops 4 and 4 units of 2 samples */
	p.width = 2312; p.height = 1448;
	len = h264_write_parameter_sets(&p, buf, sizeof buf);
	n = split(buf, len, nals, 4);
	CHECK(n == 2);
	rlen = unescape(&nals[0], rbsp);
	b = (struct br){ rbsp, rlen, 0 };
	rbits(&b, 24); rue(&b); rue(&b); rue(&b); rue(&b); rbits(&b, 2); rue(&b); rue(&b); rue(&b); rbit(&b);
	CHECK(rue(&b) == 145 - 1);
	CHECK(rue(&b) == 91 - 1);
	rbit(&b); rbit(&b);
	CHECK(rbit(&b) == 1);              /* cropping */
	CHECK(rue(&b) == 0 && rue(&b) == 4 && rue(&b) == 0 && rue(&b) == 4);

	/* sizes that are not a multiple of 8, a bad reference count and tiny buffers are refused */
	p = params();
	p.width = 2300;
	CHECK(h264_write_parameter_sets(&p, buf, sizeof buf) == 0);
	p = params();
	p.max_num_ref_frames = 0;
	CHECK(h264_write_parameter_sets(&p, buf, sizeof buf) == 0);
	p.max_num_ref_frames = 17;
	CHECK(h264_write_parameter_sets(&p, buf, sizeof buf) == 0);
	p = params();
	CHECK(h264_write_parameter_sets(&p, buf, 30) == 0);

	/* sizes whose bytes force emulation prevention still frame correctly */
	for (unsigned s = 1024; s <= 4096; s += 128) {
		p.width = s; p.height = s;
		len = h264_write_parameter_sets(&p, buf, sizeof buf);
		CHECK(len > 0);
		n = split(buf, len, nals, 4);
		CHECK(n == 2);
		for (int i = 0; i < n; i++) CHECK(no_start_code_emulation(&nals[i]));
	}
}

static void test_slice_headers(void)
{
	struct h264_params p = params();
	uint8_t buf[64], rbsp[64];
	struct nal nals[2];

	/* IDR: type 5, frame_num 0, the given idr_pic_id */
	size_t len = h264_write_slice_header(&p, true, 0, 3, 0, 0, buf, sizeof buf);
	CHECK(len > 5 && len < 16);
	CHECK(split(buf, len, nals, 2) == 1 && nals[0].type == 5 && nals[0].ref_idc == 3);
	CHECK(no_start_code_emulation(&nals[0]));
	size_t rlen = unescape(&nals[0], rbsp);
	struct br b = { rbsp, rlen, 0 };
	CHECK(rue(&b) == 0);                         /* first_mb */
	CHECK(rue(&b) == 7);                         /* I, all slices */
	CHECK(rue(&b) == 0);                         /* pps */
	CHECK(rbits(&b, H264_LOG2_MAX_FRAME_NUM) == 0);
	CHECK(rue(&b) == 3);                         /* idr_pic_id */

	/* P slice on the previous frame: no modification */
	len = h264_write_slice_header(&p, false, 300, 0, 1, 0, buf, sizeof buf);
	CHECK(len > 4 && split(buf, len, nals, 2) == 1 && nals[0].type == 1 && nals[0].ref_idc != 0);
	rlen = unescape(&nals[0], rbsp);
	b = (struct br){ rbsp, rlen, 0 };
	CHECK(rue(&b) == 0 && rue(&b) == 5 && rue(&b) == 0);
	CHECK(rbits(&b, H264_LOG2_MAX_FRAME_NUM) == 300 % 256); /* frame_num wraps */
	CHECK(rbit(&b) == 0);                        /* no override */
	CHECK(rbit(&b) == 0);                        /* no list modification */
	size_t plain_bits = b.pos;

	/* P slice on a frame 4 back: modification with abs_diff_pic_num_minus1 = 3, then the end marker */
	len = h264_write_slice_header(&p, false, 10, 0, 4, 0, buf, sizeof buf);
	CHECK(len > 4 && split(buf, len, nals, 2) == 1 && no_start_code_emulation(&nals[0]));
	rlen = unescape(&nals[0], rbsp);
	b = (struct br){ rbsp, rlen, 0 };
	CHECK(rue(&b) == 0 && rue(&b) == 5 && rue(&b) == 0);
	CHECK(rbits(&b, H264_LOG2_MAX_FRAME_NUM) == 10);
	CHECK(rbit(&b) == 0);                        /* no override */
	CHECK(rbit(&b) == 1);                        /* list modification */
	CHECK(rue(&b) == 0);                         /* subtract */
	CHECK(rue(&b) == 3);
	CHECK(rue(&b) == 3);                         /* end */
	CHECK(rbit(&b) == 0);                        /* sliding window marking */
	CHECK(rue(&b) == 0);                         /* cabac_init_idc */
	CHECK(rue(&b) == 0);                         /* slice_qp_delta = 0 */
	CHECK(b.pos > plain_bits);
	while (b.pos % 8) CHECK(rbit(&b) == 1);      /* cabac_alignment_one_bit */

	/* distance 0 means the previous frame as well */
	uint8_t buf2[64];
	size_t len0 = h264_write_slice_header(&p, false, 10, 0, 0, 0, buf, sizeof buf);
	size_t len1 = h264_write_slice_header(&p, false, 10, 0, 1, 0, buf2, sizeof buf2);
	CHECK(len0 == len1 && memcmp(buf, buf2, len0) == 0);

	/* later slices carry their start address; an address past the picture is refused */
	unsigned mbs = h264_mb_columns(&p) * h264_mb_rows(&p);
	CHECK(mbs == 144 * 90);
	len = h264_write_slice_header(&p, false, 5, 0, 1, 144 * 10, buf, sizeof buf);
	CHECK(len > 4 && split(buf, len, nals, 2) == 1 && no_start_code_emulation(&nals[0]));
	rlen = unescape(&nals[0], rbsp);
	b = (struct br){ rbsp, rlen, 0 };
	CHECK(rue(&b) == 1440);
	CHECK(h264_write_slice_header(&p, true, 0, 0, 0, mbs, buf, sizeof buf) == 0);

	/* too small a buffer, and a reference further back than frame_num can name */
	CHECK(h264_write_slice_header(&p, false, 1, 0, 1, 0, buf, 6) == 0);
	CHECK(h264_write_slice_header(&p, false, 1, 0, 256, 0, buf, sizeof buf) == 0);

	/* every frame number frames correctly (emulation prevention) */
	for (unsigned fn = 0; fn < 600; fn++)
		for (unsigned d = 0; d < 7; d++) {
			len = h264_write_slice_header(&p, false, fn, 0, d, 0, buf, sizeof buf);
			CHECK(len > 0 && split(buf, len, nals, 2) == 1 && no_start_code_emulation(&nals[0]));
		}
}

int main(void)
{
	test_level();
	test_parameter_sets();
	test_slice_headers();
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	puts("h264_headers: all checks passed");
	return 0;
}
