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

#endif
