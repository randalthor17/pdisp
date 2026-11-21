/* Unit tests for the EDID generator. The decoder here is written independently of edid.c so the tests
 * check the bytes against the EDID spec fields, not against the encoder's own logic. */
#include "edid.h"

#include <errno.h>
#include <math.h>
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

static struct edid_params base_params(void)
{
	struct edid_params p = { .width = 2304, .height = 1440, .refresh_mhz = 90000, .mm_width = 234, .mm_height = 150,
				 .pnp_id = "PDI", .product_code = 0x0102, .serial = 0xdeadbeef, .name = "pdisp-tablet",
				 .serial_text = "PDISP0001", .year = 2026 };
	return p;
}

static int checksum_ok(const uint8_t *e)
{
	unsigned sum = 0;
	for (int i = 0; i < EDID_SIZE; i++) sum += e[i];
	return (sum & 0xff) == 0;
}

static void test_fixed_fields(void)
{
	struct edid_params p = base_params();
	uint8_t e[EDID_SIZE];
	CHECK(edid_generate(&p, e) == 0);
	static const uint8_t hdr[8] = { 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0 };
	CHECK(memcmp(e, hdr, 8) == 0);
	CHECK(checksum_ok(e));

	/* manufacturer: 3 letters, 5 bits each, A=1, big-endian */
	unsigned m = (e[8] << 8) | e[9];
	CHECK(((m >> 10) & 31) == 'P' - '@' && ((m >> 5) & 31) == 'D' - '@' && (m & 31) == 'I' - '@');
	CHECK((e[10] | e[11] << 8) == 0x0102);
	CHECK((uint32_t)(e[12] | e[13] << 8 | e[14] << 16 | (uint32_t)e[15] << 24) == 0xdeadbeef);
	CHECK(e[17] == 2026 - 1990);
	CHECK(e[18] == 1 && e[19] == 4);
	CHECK(e[20] & 0x80); /* digital */
	CHECK(e[21] == 23 && e[22] == 15); /* 234x150 mm rounds to 23x15 cm: niri reads these */
}

static void test_timing_decode(void)
{
	static const struct { unsigned w, h, mhz; } modes[] = {
		{ 2400, 1080, 60000 }, { 2304, 1440, 90000 }, { 1920, 1080, 60000 }, { 1280, 800, 60000 },
		{ 2560, 1600, 120000 }, { 1080, 2400, 119880 }, { 3840, 2160, 60000 }, { 1080, 2400, 144000 },
	};
	for (size_t i = 0; i < sizeof modes / sizeof *modes; i++) {
		struct edid_params p = base_params();
		p.width = modes[i].w;
		p.height = modes[i].h;
		p.refresh_mhz = modes[i].mhz;
		uint8_t e[EDID_SIZE];
		CHECK(edid_generate(&p, e) == 0);
		CHECK(checksum_ok(e));

		const uint8_t *d = e + 54;
		unsigned clk = (d[0] | d[1] << 8) * 10; /* kHz */
		unsigned ha = d[2] | (d[4] >> 4) << 8, hb = d[3] | (d[4] & 15) << 8;
		unsigned va = d[5] | (d[7] >> 4) << 8, vb = d[6] | (d[7] & 15) << 8;
		unsigned hfp = d[8] | (d[11] >> 6) << 8, hsw = d[9] | ((d[11] >> 4) & 3) << 8;
		unsigned vfp = (d[10] >> 4) | ((d[11] >> 2) & 3) << 4, vsw = (d[10] & 15) | (d[11] & 3) << 4;
		unsigned mw = d[12] | (d[14] >> 4) << 8, mh = d[13] | (d[14] & 15) << 8;
		CHECK(ha == modes[i].w && va == modes[i].h);
		CHECK(hb == 160 && hfp == 48 && hsw == 32); /* CVT reduced blanking */
		CHECK(vfp == 3 && vsw >= 4 && vsw <= 10 && vb >= vfp + vsw + 6);
		CHECK(mw == 234 && mh == 150);
		double hz = clk * 1000.0 / ((double)(ha + hb) * (va + vb));
		CHECK(fabs(hz - modes[i].mhz / 1000.0) < 0.1);
		CHECK((d[17] & 0x80) == 0 && (d[17] & 0x18) == 0x18); /* not interlaced, digital separate sync */

		/* range limits must contain the mode */
		const uint8_t *r = e + 90;
		CHECK(r[3] == 0xfd);
		unsigned vmin = r[5] + ((r[4] & 2) ? 255 : 0), vmax = r[6] + ((r[4] & 1) ? 255 : 0);
		unsigned hmin = r[7] + ((r[4] & 8) ? 255 : 0), hmax = r[8] + ((r[4] & 4) ? 255 : 0);
		double hf = clk / (double)(ha + hb);
		CHECK(vmin <= hz && vmax >= hz);
		CHECK(hmin <= hf && hmax >= hf);
		CHECK(r[9] * 10000u >= clk);
	}
}

static void test_text_descriptors(void)
{
	struct edid_params p = base_params();
	uint8_t e[EDID_SIZE];
	snprintf(p.name, sizeof p.name, "abc");
	CHECK(edid_generate(&p, e) == 0);
	CHECK(e[72] == 0 && e[73] == 0 && e[74] == 0 && e[75] == 0xfc && e[76] == 0);
	CHECK(memcmp(e + 77, "abc\n", 4) == 0);
	CHECK(memcmp(e + 81, "         ", 9) == 0); /* space padding after the 0x0a terminator */
	CHECK(e[108 + 3] == 0xff);
	CHECK(memcmp(e + 113, "PDISP0001\n", 10) == 0);

	/* exactly 13 characters: no terminator, fills the descriptor */
	snprintf(p.name, sizeof p.name, "0123456789abc");
	CHECK(edid_generate(&p, e) == 0);
	CHECK(memcmp(e + 77, "0123456789abc", 13) == 0);
}

static void test_rejects_bad_input(void)
{
	uint8_t e[EDID_SIZE], before[EDID_SIZE];
	memset(e, 0x5a, sizeof e);
	memcpy(before, e, sizeof e);

	struct edid_params p = base_params();
	p.width = 5000; CHECK(edid_generate(&p, e) == -EINVAL);
	p = base_params(); p.width = 32; CHECK(edid_generate(&p, e) == -EINVAL);
	p = base_params(); p.refresh_mhz = 10000; CHECK(edid_generate(&p, e) == -EINVAL);
	p = base_params(); p.refresh_mhz = 400000; CHECK(edid_generate(&p, e) == -EINVAL);
	p = base_params(); p.mm_width = 0; CHECK(edid_generate(&p, e) == -EINVAL);
	p = base_params(); p.mm_height = 5000; CHECK(edid_generate(&p, e) == -EINVAL);
	p = base_params(); p.pnp_id[0] = 'a'; CHECK(edid_generate(&p, e) == -EINVAL);
	p = base_params(); p.pnp_id[1] = '1'; CHECK(edid_generate(&p, e) == -EINVAL);
	p = base_params(); p.name[0] = '\0'; CHECK(edid_generate(&p, e) == -EINVAL);
	p = base_params(); memset(p.name, 'x', EDID_TEXT_MAX); p.name[EDID_TEXT_MAX] = 'x'; CHECK(edid_generate(&p, e) == -EINVAL);
	p = base_params(); p.serial_text[0] = '\n'; CHECK(edid_generate(&p, e) == -EINVAL);
	p = base_params(); p.year = 1985; CHECK(edid_generate(&p, e) == -EINVAL);
	/* a pixel clock above 655.35 MHz cannot be encoded in one detailed timing (would need an extension block) */
	p = base_params(); p.width = 4095; p.height = 4095; p.refresh_mhz = 120000; CHECK(edid_generate(&p, e) == -EINVAL);
	p = base_params(); p.width = 2960; p.height = 1848; p.refresh_mhz = 120000; CHECK(edid_generate(&p, e) == -EINVAL);

	CHECK(memcmp(e, before, sizeof e) == 0); /* nothing written on failure */
}

static void test_deterministic_and_distinct_serial(void)
{
	struct edid_params p = base_params();
	uint8_t a[EDID_SIZE], b[EDID_SIZE], c[EDID_SIZE];
	CHECK(edid_generate(&p, a) == 0 && edid_generate(&p, b) == 0);
	CHECK(memcmp(a, b, EDID_SIZE) == 0);
	p.serial = 2;
	snprintf(p.serial_text, sizeof p.serial_text, "PDISP0002");
	CHECK(edid_generate(&p, c) == 0);
	CHECK(memcmp(a, c, EDID_SIZE) != 0 && checksum_ok(c));
}

int main(void)
{
	test_fixed_fields();
	test_timing_decode();
	test_text_descriptors();
	test_rejects_bad_input();
	test_deterministic_and_distinct_serial();
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	puts("edid: all checks passed");
	return 0;
}
