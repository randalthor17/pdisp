/* HEVC hardware encoder (VAAPI), driven directly through libva.
 *
 * Input is a BGRA/BGRX frame in system memory. It is uploaded to a VA surface, converted to NV12 on the GPU by the
 * VA video-processing pipeline (BT.709, full range in, limited range out) and encoded as HEVC Main 8-bit:
 * no B-frames, one reference frame, CBR or VBR with a small VBV buffer. The first frame and any frame with `force_idr` are
 * IDR; there is no periodic keyframe. One slice per picture.
 */
#ifndef PDISP_ENCODER_H
#define PDISP_ENCODER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct encoder;

struct encoder_config {
	const char *render_node; /* e.g. /dev/dri/renderD128; NULL picks the first node that can encode HEVC */
	unsigned width;          /* multiple of 8 */
	unsigned height;         /* multiple of 8 */
	unsigned fps;            /* nominal frame rate, used for rate control */
	unsigned bitrate_kbps;   /* CBR: the rate. VBR: the peak rate the buffer is sized for */
	bool vbr;                /* false: CBR (pads to the rate); true: VBR with an average of target_percent of the peak */
	unsigned target_percent; /* VBR average as a percentage of bitrate_kbps (default 50) */
	unsigned vbv_ms;         /* rate-control buffer length; small keeps keyframes close to P-frame size */
	unsigned slices;         /* slices per picture (default 1); each is an independently decodable run of CTU rows */
	unsigned intra_refresh_frames; /* 0: off. Otherwise a rolling column refresh sweeps the picture in about this many
	                                  frames, so decoding recovers from lost frames without an IDR */
};

struct encoder_packet {
	const uint8_t *data; /* Annex B bitstream; valid until the next encoder_encode() or encoder_close() */
	size_t size;
	bool idr;
	uint64_t frame_index; /* 0-based count of frames since open; the handle for encoder_invalidate_from() */
	int ref_distance;     /* how many frames back the reference is (1 = the previous frame); 0 for an IDR */
	double upload_ms;  /* BGRA to VA surface */
	double convert_ms; /* RGB to NV12 on the GPU */
	double encode_ms;  /* submit until the coded buffer is ready */
};

/* Returns 0, or -errno / -EIO. `*out` is set on success. */
int encoder_open(const struct encoder_config *cfg, struct encoder **out);

/* Name of the VA driver in use, for logs. */
const char *encoder_description(const struct encoder *enc);

/* Encode one frame. `stride` is bytes per row of `bgra`. Returns 0, or a negative errno. */
int encoder_encode(struct encoder *enc, const uint8_t *bgra, int stride, bool force_idr, struct encoder_packet *out);

/* The receiver did not get frames from `first_lost` on (and may not have decoded anything after them). From the
 * next frame the encoder predicts only from frames before `first_lost`, so the receiver's picture is correct again
 * without an IDR. If no earlier frame is still held, the next frame is an IDR. */
void encoder_invalidate_from(struct encoder *enc, uint64_t first_lost);

void encoder_close(struct encoder *enc);

#endif
