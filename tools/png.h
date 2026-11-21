/* Minimal PNG writer for debugging and test artifacts (8-bit RGB, no interlace). */
#ifndef PDISP_PNG_H
#define PDISP_PNG_H

#include <stdint.h>

/* Write a BGRA/BGRX frame (B first, `stride` bytes per row) as an RGB PNG. Returns 0, or -1 on error. */
int png_write_bgra(const char *path, const uint8_t *bgra, int width, int height, int stride);

#endif
