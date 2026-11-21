#include "edid.h"

#include <errno.h>
#include <math.h>
#include <string.h>

#define CVT_RB_HBLANK 160
#define CVT_RB_HFRONT 48
#define CVT_RB_HSYNC 32
#define CVT_RB_VFRONT 3
#define CVT_RB_MIN_VBPORCH 6
#define CVT_RB_MIN_VBLANK_US 460.0

static unsigned vsync_lines(unsigned w, unsigned h)
{
	/* CVT vertical sync width by aspect ratio; custom ratios use 10 */
	if (w * 3 == h * 4) return 4;
	if (w * 9 == h * 16) return 5;
	if (w * 10 == h * 16) return 6;
	if (w * 4 == h * 5 || w * 9 == h * 15) return 7;
	return 10;
}

int edid_compute_timing(unsigned width, unsigned height, unsigned refresh_mhz, struct edid_timing *out)
{
	if (width < 64 || width > 4095 || height < 64 || height > 4095) return -EINVAL;
	if (refresh_mhz < 24000 || refresh_mhz > 255000) return -EINVAL;

	double hz = refresh_mhz / 1000.0;
	unsigned vsync = vsync_lines(width, height);

	/* CVT-RB v1: the vertical blank must last at least 460 us */
	double h_period = (1.0 / hz - CVT_RB_MIN_VBLANK_US / 1e6) / height;
	unsigned vbi = (unsigned)floor(CVT_RB_MIN_VBLANK_US / 1e6 / h_period) + 1;
	unsigned min_vbi = CVT_RB_VFRONT + vsync + CVT_RB_MIN_VBPORCH;
	if (vbi < min_vbi) vbi = min_vbi;

	unsigned htotal = width + CVT_RB_HBLANK;
	unsigned vtotal = height + vbi;
	/* EDID stores the pixel clock in 10 kHz units; round to nearest */
	double clk_khz = hz * htotal * vtotal / 1000.0;
	unsigned clk_10khz = (unsigned)lround(clk_khz / 10.0);
	if (clk_10khz < 1 || clk_10khz > 65535 || vbi > 4095) return -EINVAL;

	out->hactive = width;
	out->hblank = CVT_RB_HBLANK;
	out->hfront = CVT_RB_HFRONT;
	out->hsync = CVT_RB_HSYNC;
	out->vactive = height;
	out->vblank = vbi;
	out->vfront = CVT_RB_VFRONT;
	out->vsync = vsync;
	out->pixel_clock_khz = clk_10khz * 10;
	out->refresh_hz = out->pixel_clock_khz * 1000.0 / ((double)htotal * vtotal);
	return 0;
}

static int valid_text(const char *s)
{
	size_t n = strnlen(s, EDID_TEXT_MAX + 1);
	if (n == 0 || n > EDID_TEXT_MAX) return 0;
	for (size_t i = 0; i < n; i++)
		if (s[i] < 0x20 || s[i] > 0x7e) return 0;
	return 1;
}

static void put_text_descriptor(uint8_t *d, uint8_t tag, const char *s)
{
	size_t n = strlen(s);
	memset(d, 0, 18);
	d[3] = tag;
	memset(d + 5, 0x20, 13);
	memcpy(d + 5, s, n);
	if (n < 13) d[5 + n] = 0x0a; /* terminator, then space padding */
}

int edid_generate(const struct edid_params *p, uint8_t out[EDID_SIZE])
{
	struct edid_timing t;
	uint8_t e[EDID_SIZE] = { 0 };

	if (p->mm_width < 10 || p->mm_width > 2550 || p->mm_height < 10 || p->mm_height > 2550) return -EINVAL;
	if (p->year < 1990 || p->year > 1990 + 255) return -EINVAL;
	if (!valid_text(p->name) || !valid_text(p->serial_text)) return -EINVAL;
	for (int i = 0; i < 3; i++)
		if (p->pnp_id[i] < 'A' || p->pnp_id[i] > 'Z') return -EINVAL;
	if (p->pnp_id[3] != '\0') return -EINVAL;
	if (edid_compute_timing(p->width, p->height, p->refresh_mhz, &t) != 0) return -EINVAL;

	/* header, vendor, product, serial, date, version 1.4 */
	static const uint8_t header[8] = { 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00 };
	memcpy(e, header, 8);
	unsigned pnp = ((p->pnp_id[0] - '@') << 10) | ((p->pnp_id[1] - '@') << 5) | (p->pnp_id[2] - '@');
	e[8] = pnp >> 8;
	e[9] = pnp & 0xff;
	e[10] = p->product_code & 0xff;
	e[11] = p->product_code >> 8;
	for (int i = 0; i < 4; i++) e[12 + i] = (p->serial >> (8 * i)) & 0xff;
	e[16] = 1; /* week */
	e[17] = p->year - 1990;
	e[18] = 1;
	e[19] = 4;

	/* basic display parameters: digital, 8 bpc, DisplayPort; size in cm; gamma 2.2; sRGB, preferred = native */
	e[20] = 0xa5;
	e[21] = (p->mm_width + 5) / 10;
	e[22] = (p->mm_height + 5) / 10;
	e[23] = 120;
	e[24] = 0x06;

	/* sRGB chromaticity */
	static const uint8_t chroma[10] = { 0xee, 0x91, 0xa3, 0x54, 0x4c, 0x99, 0x26, 0x0f, 0x50, 0x54 };
	memcpy(e + 25, chroma, sizeof chroma);

	/* no established timings; standard timings unused (0x0101) */
	for (int i = 0; i < 8; i++) {
		e[38 + 2 * i] = 0x01;
		e[39 + 2 * i] = 0x01;
	}

	/* detailed timing descriptor: +hsync, -vsync (CVT-RB) */
	uint8_t *d = e + 54;
	unsigned clk = t.pixel_clock_khz / 10;
	d[0] = clk & 0xff;
	d[1] = clk >> 8;
	d[2] = t.hactive & 0xff;
	d[3] = t.hblank & 0xff;
	d[4] = ((t.hactive >> 8) << 4) | (t.hblank >> 8);
	d[5] = t.vactive & 0xff;
	d[6] = t.vblank & 0xff;
	d[7] = ((t.vactive >> 8) << 4) | (t.vblank >> 8);
	d[8] = t.hfront & 0xff;
	d[9] = t.hsync & 0xff;
	d[10] = ((t.vfront & 0xf) << 4) | (t.vsync & 0xf);
	d[11] = ((t.hfront >> 8) << 6) | ((t.hsync >> 8) << 4) | ((t.vfront >> 4) << 2) | (t.vsync >> 4);
	d[12] = p->mm_width & 0xff;
	d[13] = p->mm_height & 0xff;
	d[14] = ((p->mm_width >> 8) << 4) | (p->mm_height >> 8);
	d[17] = 0x1a; /* digital separate sync, hsync+, vsync- */

	put_text_descriptor(e + 72, 0xfc, p->name);

	/* range limits: cover the chosen mode, with the pixel clock rounded up to 10 MHz */
	unsigned htotal = t.hactive + t.hblank;
	unsigned hfreq_khz = (unsigned)ceil(t.pixel_clock_khz / (double)htotal);
	unsigned vmax = (unsigned)ceil(t.refresh_hz);
	unsigned vmin = vmax < 30 ? vmax : 30;
	unsigned hmin = hfreq_khz < 30 ? hfreq_khz : 30;
	unsigned hmax = hfreq_khz + 1;
	unsigned clk_max = t.pixel_clock_khz / 10000 + 1;
	/* EDID 1.4 offset flags (byte 4) extend each limit by 255 when its byte would overflow */
	uint8_t flags = 0;
	if (vmax > 255) { flags |= 0x01; vmax -= 255; if (vmin > 255) { flags |= 0x02; vmin -= 255; } }
	if (hmax > 255) { flags |= 0x04; hmax -= 255; if (hmin > 255) { flags |= 0x08; hmin -= 255; } }
	if (vmax > 255 || hmax > 255 || clk_max > 255) return -EINVAL;
	uint8_t *r = e + 90;
	r[3] = 0xfd;
	r[4] = flags;
	r[5] = vmin;
	r[6] = vmax;
	r[7] = hmin;
	r[8] = hmax;
	r[9] = clk_max;
	r[10] = 0x01; /* range limits only, no secondary timing formula */
	r[11] = 0x0a;
	memset(r + 12, 0x20, 6);

	put_text_descriptor(e + 108, 0xff, p->serial_text);

	uint8_t sum = 0;
	for (int i = 0; i < EDID_SIZE - 1; i++) sum += e[i];
	e[EDID_SIZE - 1] = (uint8_t)(256 - sum);

	memcpy(out, e, EDID_SIZE);
	return 0;
}
