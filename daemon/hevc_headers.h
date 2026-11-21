/* HEVC parameter sets (VPS, SPS, PPS) for the encoder.
 *
 * Mesa's VAAPI encoder does not emit parameter sets itself; the application supplies them as a packed sequence
 * header. The values mirror what the driver codes (32x32 CTUs, 8x8 minimum CB, AMP on, SAO and temporal MVP off,
 * no short-term RPS in the SPS, 12-bit POC LSB) and must stay in step with the VA sequence/picture parameters
 * in encoder.c. The SPS VUI carries the colour description so decoders interpret the stream correctly.
 */
#ifndef PDISP_HEVC_HEADERS_H
#define PDISP_HEVC_HEADERS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct hevc_params {
	unsigned width;  /* luma samples, multiple of 8 (no conformance window) */
	unsigned height;
	unsigned fps;    /* nominal, for the timing fields */
	unsigned level_idc; /* 30 * level: 150 = 5.0, 153 = 5.1, 156 = 5.2 */
	int init_qp;     /* 26 + init_qp_minus26 */
	bool full_range;
	uint8_t colour_primaries;         /* H.273 code points, e.g. 1 = BT.709 */
	uint8_t transfer_characteristics; /* 13 = sRGB / IEC 61966-2-1 */
	uint8_t matrix_coefficients;      /* 1 = BT.709 */
};

/* Writes VPS, SPS and PPS as Annex B (4-byte start codes, emulation prevention applied).
 * Returns the number of bytes, or 0 if `cap` is too small or a parameter cannot be coded. */
size_t hevc_write_parameter_sets(const struct hevc_params *p, uint8_t *out, size_t cap);

/* Writes the slice segment header NAL for a single-slice picture, as Annex B (start code, NAL header, header bits,
 * byte alignment). IDR pictures use IDR_W_RADL; others are TRAIL_R P-slices that reference the previous picture
 * (POC - 1). `poc` is the picture order count since the last IDR. Returns the number of bytes, or 0 on error.
 * The driver parses this to learn the slice parameters; the picture data itself is produced by the hardware. */
size_t hevc_write_slice_header(const struct hevc_params *p, bool idr, unsigned poc, uint8_t *out, size_t cap);

#endif
