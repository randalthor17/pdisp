#include "png.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

static void put_be32(uint8_t *p, uint32_t v)
{
	p[0] = v >> 24;
	p[1] = v >> 16;
	p[2] = v >> 8;
	p[3] = v;
}

static int write_chunk(FILE *f, const char type[4], const uint8_t *data, uint32_t len)
{
	uint8_t hdr[8];
	put_be32(hdr, len);
	memcpy(hdr + 4, type, 4);
	uLong crc = crc32(0, (const Bytef *)type, 4);
	if (len) crc = crc32(crc, data, len);
	uint8_t tail[4];
	put_be32(tail, (uint32_t)crc);
	return fwrite(hdr, 1, 8, f) == 8 && (len == 0 || fwrite(data, 1, len, f) == len) && fwrite(tail, 1, 4, f) == 4 ? 0 : -1;
}

int png_write_bgra(const char *path, const uint8_t *bgra, int width, int height, int stride)
{
	size_t row = 1 + (size_t)width * 3; /* filter byte + RGB */
	size_t raw_len = row * height;
	uint8_t *raw = malloc(raw_len);
	if (!raw) return -1;
	for (int y = 0; y < height; y++) {
		uint8_t *o = raw + row * y;
		const uint8_t *s = bgra + (size_t)stride * y;
		*o++ = 0; /* filter: none */
		for (int x = 0; x < width; x++, s += 4) {
			*o++ = s[2];
			*o++ = s[1];
			*o++ = s[0];
		}
	}
	uLongf z_len = compressBound(raw_len);
	uint8_t *z = malloc(z_len);
	if (!z || compress2(z, &z_len, raw, raw_len, 1) != Z_OK) {
		free(raw);
		free(z);
		return -1;
	}
	free(raw);

	FILE *f = fopen(path, "wb");
	int rc = -1;
	if (f) {
		static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };
		uint8_t ihdr[13];
		put_be32(ihdr, (uint32_t)width);
		put_be32(ihdr + 4, (uint32_t)height);
		ihdr[8] = 8; /* bit depth */
		ihdr[9] = 2; /* RGB */
		ihdr[10] = ihdr[11] = ihdr[12] = 0;
		rc = fwrite(sig, 1, 8, f) == 8 && write_chunk(f, "IHDR", ihdr, 13) == 0 &&
		     write_chunk(f, "IDAT", z, (uint32_t)z_len) == 0 && write_chunk(f, "IEND", NULL, 0) == 0
			     ? 0
			     : -1;
		if (fclose(f) != 0) rc = -1;
	}
	free(z);
	return rc;
}
