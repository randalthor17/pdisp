/* Bit writer for the parameter set and slice header writers (hevc_headers.c, h264_headers.c): fixed-width and Exp-Golomb codes into a buffer,
 * with an overflow flag instead of a crash when the buffer is too small. */
#ifndef PDISP_BITS_H
#define PDISP_BITS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct bits {
	uint8_t *buf;
	size_t cap;
	size_t pos; /* in bits */
	bool overflow;
};

static inline void put_bit(struct bits *b, unsigned v)
{
	if (b->pos / 8 >= b->cap) { b->overflow = true; return; }
	if ((b->pos & 7) == 0) b->buf[b->pos / 8] = 0;
	if (v) b->buf[b->pos / 8] |= (uint8_t)(0x80 >> (b->pos & 7));
	b->pos++;
}

static inline void u(struct bits *b, unsigned n, uint32_t v)
{
	for (unsigned i = n; i-- > 0;) put_bit(b, (v >> i) & 1);
}

static inline void ue(struct bits *b, uint32_t v)
{
	uint64_t x = (uint64_t)v + 1;
	unsigned len = 0;
	while ((x >> len) > 1) len++;
	for (unsigned i = 0; i < len; i++) put_bit(b, 0);
	for (unsigned i = len + 1; i-- > 0;) put_bit(b, (x >> i) & 1);
}

static inline void se(struct bits *b, int32_t v)
{
	ue(b, v > 0 ? (uint32_t)(2 * v - 1) : (uint32_t)(-2 * v));
}

/* rbsp_trailing_bits(): a 1 bit, then zeros to the byte boundary */
static inline void trailing_bits(struct bits *b)
{
	put_bit(b, 1);
	while (b->pos & 7) put_bit(b, 0);
}

#endif
