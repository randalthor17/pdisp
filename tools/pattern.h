/* Deterministic test pattern shared by the pattern client (which draws it) and the capture tool (which
 * checks it). Every pixel is a pure function of its coordinates and the output size, so a captured frame can be
 * verified bit-exactly without keeping a reference image.
 *
 *   R: horizontal ramp, G: vertical ramp, B: 32 px checkerboard, 2 px white border.
 */
#ifndef PDISP_PATTERN_H
#define PDISP_PATTERN_H

#include <stdint.h>

/* Returns 0x00RRGGBB for pixel (x, y) of a w x h frame. */
static inline uint32_t pattern_pixel(unsigned x, unsigned y, unsigned w, unsigned h)
{
	if (x < 2 || y < 2 || x >= w - 2 || y >= h - 2) return 0x00ffffffu;
	uint32_t r = x * 255u / (w - 1);
	uint32_t g = y * 255u / (h - 1);
	uint32_t b = (((x >> 5) ^ (y >> 5)) & 1u) ? 255u : 0u;
	return (r << 16) | (g << 8) | b;
}

/* A static page that stands in for text (the quality check of M10). Glyph-like cells (8x14 px in a 10x20 cell, 5x9 grid cells that do not fall on
 * pixel boundaries, so their edges are anti-aliased grey as real text is) in four bands: black on white, white on dark, coloured on white and
 * light grey on white (low contrast). The glyphs are hashed noise, not letters: denser in detail than real text, so a result here is a lower
 * bound. Every pixel is a pure function of its coordinates, like pattern_pixel. */
static inline uint32_t textpage_hash(uint32_t v)
{
	v ^= v >> 16; v *= 0x7feb352du; v ^= v >> 15; v *= 0x846ca68bu; v ^= v >> 16;
	return v;
}

static inline int textpage_on(unsigned cell_x, unsigned line, int gx, int gy)
{
	uint32_t h = textpage_hash(cell_x * 7919u + line * 104729u + 17u);
	if (h % 7 == 0) return 0; /* a space now and then */
	uint32_t bits = textpage_hash(h + (uint32_t)gy * 31u);
	/* 5 columns; a cell is on with probability 3/8, and in a stroke-like run when its left neighbour is on */
	uint32_t left = gx > 0 ? ((textpage_hash(h + (uint32_t)gy * 31u + (uint32_t)(gx - 1) * 977u) >> 3) % 8 < 3) : 0;
	uint32_t me = (textpage_hash(h + (uint32_t)gy * 31u + (uint32_t)gx * 977u) >> 3) % 8 < (left ? 5u : 2u);
	(void)bits;
	return (int)me;
}

static inline uint32_t textpage_pixel(unsigned x, unsigned y, unsigned w, unsigned h)
{
	(void)w;
	unsigned line = y / 20, band = (line / 12) % 4;
	uint32_t bg, fg;
	switch (band) {
	case 0: bg = 0x00ffffffu; fg = 0x00000000u; break;
	case 1: bg = 0x001e1e2eu; fg = 0x00ffffffu; break;
	case 2: {
		static const uint32_t col[4] = { 0x00c00000u, 0x00008000u, 0x000000c0u, 0x00000000u };
		bg = 0x00ffffffu; fg = col[(line * 5u + 1u) % 4u];
		break;
	}
	default: bg = 0x00ffffffu; fg = 0x00a0a0a0u; break;
	}
	if (y + 2 >= h) return bg;
	unsigned cell_x = x / 10, ox = x % 10, oy = y % 20;
	unsigned cov = 0; /* 2x2 samples */
	for (unsigned sy = 0; sy < 2; sy++)
		for (unsigned sx = 0; sx < 2; sx++) {
			double px = (double)ox + 0.25 + 0.5 * sx - 1.0, py = (double)oy + 0.25 + 0.5 * sy - 3.0;
			if (px < 0 || py < 0 || px >= 8.0 || py >= 14.0) continue;
			cov += (unsigned)textpage_on(cell_x, line, (int)(px * 5.0 / 8.0), (int)(py * 9.0 / 14.0));
		}
	if (!cov) return bg;
	uint32_t out = 0;
	for (int c = 0; c < 3; c++) {
		uint32_t a = (bg >> (16 - 8 * c)) & 0xff, b = (fg >> (16 - 8 * c)) & 0xff;
		out |= ((a * (4 - cov) + b * cov) / 4) << (16 - 8 * c);
	}
	return out;
}

#endif
