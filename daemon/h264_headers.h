/* H.264 parameter sets (SPS, PPS) and slice headers for the encoder, the counterpart of hevc_headers.h.
 *
 * Mesa's VAAPI encoder takes the application's packed headers (see hevc_headers.h). The stream is High profile, CABAC, 8x8 transform,
 * progressive, no B-frames, one reference picture per slice chosen from up to `max_num_ref_frames` retained frames:
 *   - pic_order_cnt_type 2 (output order is decode order, no reordering: POC is 2 * the frame number),
 *   - gaps_in_frame_num_value_allowed_flag 1: a receiver that never got a frame carries on with the next (the decoder fills the gap with
 *     "non-existing" frames in its sliding window), which is how the loss recovery works without an IDR,
 *   - sliding-window reference marking, so the decoder keeps the last `max_num_ref_frames` frames, as the encoder does,
 *   - a P slice predicts from the most recent frame unless the header carries a reference list modification naming an older one.
 * The values must stay in step with the VA sequence/picture parameters in encoder.c.
 */
#ifndef PDISP_H264_HEADERS_H
#define PDISP_H264_HEADERS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define H264_MB_SIZE 16
#define H264_LOG2_MAX_FRAME_NUM 8 /* frame_num is 8 bits: it wraps every 256 frames */

struct h264_params {
	unsigned width;  /* luma samples, multiple of 8 (when not a multiple of 16 the picture is cropped) */
	unsigned height;
	unsigned fps;    /* nominal, for the timing fields */
	unsigned level_idc; /* 10 * level: 40 = 4.0, 52 = 5.2, 60 = 6.0 */
	unsigned max_num_ref_frames; /* the decoder's sliding window (1 to 16) */
	int init_qp;     /* 26 + pic_init_qp_minus26 */
	bool full_range;
	uint8_t colour_primaries;         /* H.273 code points, e.g. 1 = BT.709 */
	uint8_t transfer_characteristics; /* 13 = sRGB / IEC 61966-2-1 */
	uint8_t matrix_coefficients;      /* 1 = BT.709 */
};

/* The smallest level that holds the picture size and macroblock rate. */
unsigned h264_level_idc(unsigned width, unsigned height, unsigned fps);

/* The coded picture size in macroblocks (the size rounded up to a multiple of 16). */
unsigned h264_mb_columns(const struct h264_params *p);
unsigned h264_mb_rows(const struct h264_params *p);

/* Writes SPS and PPS as Annex B (4-byte start codes, emulation prevention applied). Returns the number of bytes, or 0 if `cap` is too small or
 * a parameter cannot be coded. */
size_t h264_write_parameter_sets(const struct h264_params *p, uint8_t *out, size_t cap);

/* Writes the slice header NAL as Annex B (start code, NAL header, header bits, CABAC alignment). IDR pictures use nal_unit_type 5 and
 * `idr_pic_id` (consecutive IDRs must differ); others are P slices of type 1. `frame_num` counts frames since the IDR (it is coded modulo
 * 256). `ref_distance` is how many frames back the slice's reference is: 0 or 1 is the previous frame (the decoder's default first entry),
 * more writes a reference list modification. `first_mb` is the address of the slice's first macroblock in raster order; 0 starts the
 * picture. Returns the number of bytes, or 0 on error. The driver parses this to learn the slice parameters. */
size_t h264_write_slice_header(const struct h264_params *p, bool idr, unsigned frame_num, unsigned idr_pic_id, unsigned ref_distance,
			       unsigned first_mb, uint8_t *out, size_t cap);

#endif
