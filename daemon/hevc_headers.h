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
	unsigned dpb_size; /* decoded picture buffer size including the current picture (VPS/SPS max_dec_pic_buffering) */
	int init_qp;     /* 26 + init_qp_minus26 */
	bool full_range;
	uint8_t colour_primaries;         /* H.273 code points, e.g. 1 = BT.709 */
	uint8_t transfer_characteristics; /* 13 = sRGB / IEC 61966-2-1 */
	uint8_t matrix_coefficients;      /* 1 = BT.709 */
};

/* Writes VPS, SPS and PPS as Annex B (4-byte start codes, emulation prevention applied).
 * Returns the number of bytes, or 0 if `cap` is too small or a parameter cannot be coded. */
size_t hevc_write_parameter_sets(const struct hevc_params *p, uint8_t *out, size_t cap);

/* One picture of the reference picture set signalled in a P-slice header. */
struct hevc_ref {
	int poc;    /* picture order count of the reference */
	bool used;  /* used by the current picture; the others are only kept in the decoder's buffer */
};

/* Writes the slice segment header NAL for a single-slice picture, as Annex B (start code, NAL header, header bits,
 * byte alignment). IDR pictures use IDR_W_RADL; others are TRAIL_R P-slices. `poc` is the picture order count since
 * the last IDR. `refs` lists the reference picture set, ordered by decreasing POC (closest first, all below `poc`);
 * exactly one entry must be `used` (the single reference the slice predicts from), the rest stay in the decoder's
 * buffer so a later frame can predict from them. `slice_ctu_address` is the address of the slice's first CTU in
 * raster order; 0 starts the picture. Returns the number of bytes, or 0 on error. The driver parses this to learn the
 * slice parameters; the picture data itself is produced by the hardware. */
size_t hevc_write_slice_header(const struct hevc_params *p, bool idr, int poc, unsigned slice_ctu_address,
			       const struct hevc_ref *refs, int nrefs, uint8_t *out, size_t cap);

/* CTU size the encoder uses (32x32) and the picture size in CTUs, for slice addressing. */
#define HEVC_CTU_SIZE 32
unsigned hevc_ctu_columns(const struct hevc_params *p);
unsigned hevc_ctu_rows(const struct hevc_params *p);

#endif
