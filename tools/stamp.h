/* A timestamp drawn into the picture so that latency can be read back out of the decoded video, independent of any
 * timestamp that travels beside it. Row 0..15 of the frame carries 52 blocks of 16x16 pixels, black or white: the marker
 * 1,0,1,0 then 48 bits (most significant first) of a microsecond clock. Black and white at 16 px survive lossy coding and
 * chroma subsampling, and are read at the block centres from the luma plane.
 */
#ifndef PDISP_STAMP_H
#define PDISP_STAMP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define STAMP_BLOCK 16
#define STAMP_MARKER_BITS 4
#define STAMP_VALUE_BITS 48
#define STAMP_BITS (STAMP_MARKER_BITS + STAMP_VALUE_BITS)
#define STAMP_WIDTH (STAMP_BITS * STAMP_BLOCK)
#define STAMP_MARKER 0xAu /* 1010 */

/* The bit at position i (0 = first marker bit) of a stamp carrying `us`. */
static inline unsigned stamp_bit(uint64_t us, unsigned i)
{
	if (i < STAMP_MARKER_BITS) return (STAMP_MARKER >> (STAMP_MARKER_BITS - 1 - i)) & 1u;
	return (unsigned)((us >> (STAMP_VALUE_BITS - 1 - (i - STAMP_MARKER_BITS))) & 1u);
}

/* Reads a stamp from a luma plane (limited range: black 16, white 235). Returns false if the marker is wrong. */
static inline bool stamp_read(const uint8_t *luma, int stride, int width, uint64_t *us)
{
	if (width < STAMP_WIDTH) return false;
	uint64_t v = 0;
	unsigned marker = 0;
	for (unsigned i = 0; i < STAMP_BITS; i++) {
		unsigned bit = luma[(STAMP_BLOCK / 2) * (size_t)stride + i * STAMP_BLOCK + STAMP_BLOCK / 2] > 125;
		if (i < STAMP_MARKER_BITS) marker = marker << 1 | bit;
		else v = v << 1 | bit;
	}
	if (marker != STAMP_MARKER) return false;
	*us = v;
	return true;
}

#endif
