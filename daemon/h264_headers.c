#include "h264_headers.h"

#include "bits.h"

#include <string.h>

#define NAL_SLICE 1
#define NAL_IDR 5
#define NAL_SPS 7
#define NAL_PPS 8

unsigned h264_mb_columns(const struct h264_params *p) { return (p->width + H264_MB_SIZE - 1) / H264_MB_SIZE; }
unsigned h264_mb_rows(const struct h264_params *p) { return (p->height + H264_MB_SIZE - 1) / H264_MB_SIZE; }

unsigned h264_level_idc(unsigned w, unsigned h, unsigned fps)
{
	/* Table A-1: level, MaxMBPS (macroblocks per second), MaxFS (macroblocks per frame) */
	static const struct { unsigned level, max_mbps, max_fs; } t[] = {
		{ 40, 245760, 8192 },   { 42, 522240, 8704 },    { 50, 589824, 22080 },   { 51, 983040, 36864 },
		{ 52, 2073600, 36864 }, { 60, 4177920, 139264 }, { 61, 8355840, 139264 }, { 62, 16711680, 139264 },
	};
	unsigned long long fs = (unsigned long long)((w + 15) / 16) * ((h + 15) / 16);
	unsigned long long mbps = fs * fps;
	for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++)
		if (mbps <= t[i].max_mbps && fs <= t[i].max_fs) return t[i].level;
	return 62;
}

static void write_sps(struct bits *b, const struct h264_params *p)
{
	const unsigned mbw = h264_mb_columns(p), mbh = h264_mb_rows(p);
	const unsigned crop_x = (mbw * H264_MB_SIZE - p->width) / 2; /* in units of 2 luma samples (4:2:0) */
	const unsigned crop_y = (mbh * H264_MB_SIZE - p->height) / 2;

	u(b, 8, 100);  /* profile_idc: High */
	u(b, 8, 0x0c); /* constraint_set4 and 5 (frame only, no B slices), like the driver's own streams */
	u(b, 8, p->level_idc);
	ue(b, 0);      /* seq_parameter_set_id */
	ue(b, 1);      /* chroma_format_idc: 4:2:0 */
	ue(b, 0);      /* bit_depth_luma_minus8 */
	ue(b, 0);      /* bit_depth_chroma_minus8 */
	u(b, 1, 0);    /* qpprime_y_zero_transform_bypass_flag */
	u(b, 1, 0);    /* seq_scaling_matrix_present_flag */
	ue(b, H264_LOG2_MAX_FRAME_NUM - 4); /* log2_max_frame_num_minus4 */
	ue(b, 2);      /* pic_order_cnt_type */
	ue(b, p->max_num_ref_frames);
	u(b, 1, 1);    /* gaps_in_frame_num_value_allowed_flag */
	ue(b, mbw - 1);
	ue(b, mbh - 1); /* pic_height_in_map_units_minus1 (frame_mbs_only: map units are macroblocks) */
	u(b, 1, 1);    /* frame_mbs_only_flag */
	u(b, 1, 1);    /* direct_8x8_inference_flag */
	if (crop_x || crop_y) {
		u(b, 1, 1); /* frame_cropping_flag */
		ue(b, 0);
		ue(b, crop_x);
		ue(b, 0);
		ue(b, crop_y);
	} else {
		u(b, 1, 0);
	}
	u(b, 1, 1);    /* vui_parameters_present_flag */

	u(b, 1, 0);    /* aspect_ratio_info_present_flag */
	u(b, 1, 0);    /* overscan_info_present_flag */
	u(b, 1, 1);    /* video_signal_type_present_flag */
	u(b, 3, 5);    /* video_format: unspecified */
	u(b, 1, p->full_range);
	u(b, 1, 1);    /* colour_description_present_flag */
	u(b, 8, p->colour_primaries);
	u(b, 8, p->transfer_characteristics);
	u(b, 8, p->matrix_coefficients);
	u(b, 1, 0);    /* chroma_loc_info_present_flag */
	u(b, 1, 1);    /* timing_info_present_flag */
	u(b, 32, 1);   /* num_units_in_tick */
	u(b, 32, 2 * p->fps); /* time_scale: a tick is a field */
	u(b, 1, 0);    /* fixed_frame_rate_flag: the screen only sends what changes */
	u(b, 1, 0);    /* nal_hrd_parameters_present_flag */
	u(b, 1, 0);    /* vcl_hrd_parameters_present_flag */
	u(b, 1, 0);    /* pic_struct_present_flag */
	u(b, 1, 1);    /* bitstream_restriction_flag */
	u(b, 1, 1);    /* motion_vectors_over_pic_boundaries_flag */
	ue(b, 0);      /* max_bytes_per_pic_denom */
	ue(b, 0);      /* max_bits_per_mb_denom */
	ue(b, 15);     /* log2_max_mv_length_horizontal */
	ue(b, 15);     /* log2_max_mv_length_vertical */
	ue(b, 0);      /* max_num_reorder_frames: output at once */
	ue(b, p->max_num_ref_frames); /* max_dec_frame_buffering */
	trailing_bits(b);
}

static void write_pps(struct bits *b, const struct h264_params *p)
{
	ue(b, 0);      /* pic_parameter_set_id */
	ue(b, 0);      /* seq_parameter_set_id */
	u(b, 1, 1);    /* entropy_coding_mode_flag: CABAC */
	u(b, 1, 0);    /* bottom_field_pic_order_in_frame_present_flag */
	ue(b, 0);      /* num_slice_groups_minus1 */
	ue(b, 0);      /* num_ref_idx_l0_default_active_minus1: one reference */
	ue(b, 0);      /* num_ref_idx_l1_default_active_minus1 */
	u(b, 1, 0);    /* weighted_pred_flag */
	u(b, 2, 0);    /* weighted_bipred_idc */
	se(b, p->init_qp - 26);
	se(b, 0);      /* pic_init_qs_minus26 */
	se(b, 0);      /* chroma_qp_index_offset */
	u(b, 1, 0);    /* deblocking_filter_control_present_flag */
	u(b, 1, 0);    /* constrained_intra_pred_flag */
	u(b, 1, 0);    /* redundant_pic_cnt_present_flag */
	u(b, 1, 1);    /* transform_8x8_mode_flag */
	u(b, 1, 0);    /* pic_scaling_matrix_present_flag */
	se(b, 0);      /* second_chroma_qp_index_offset */
	trailing_bits(b);
}

/* Appends one NAL unit: start code, 1-byte header, RBSP with emulation prevention. Returns the new length, or 0. */
static size_t append_nal(uint8_t *out, size_t cap, size_t len, unsigned ref_idc, unsigned nal_type, const uint8_t *rbsp, size_t rbsp_len)
{
	if (len + 5 > cap) return 0;
	static const uint8_t sc[4] = { 0, 0, 0, 1 };
	memcpy(out + len, sc, 4);
	len += 4;
	out[len++] = (uint8_t)(ref_idc << 5 | nal_type);
	unsigned zeros = 0;
	for (size_t i = 0; i < rbsp_len; i++) {
		if (len + 2 > cap) return 0;
		if (zeros >= 2 && rbsp[i] <= 3) {
			out[len++] = 3; /* emulation prevention byte */
			zeros = 0;
		}
		out[len++] = rbsp[i];
		zeros = rbsp[i] == 0 ? zeros + 1 : 0;
	}
	return len;
}

size_t h264_write_parameter_sets(const struct h264_params *p, uint8_t *out, size_t cap)
{
	if (p->width == 0 || p->height == 0 || p->width % 8 || p->height % 8 || p->fps == 0 || p->max_num_ref_frames < 1 ||
	    p->max_num_ref_frames > 16)
		return 0;

	size_t len = 0;
	for (int i = 0; i < 2; i++) {
		uint8_t rbsp[256];
		struct bits b = { .buf = rbsp, .cap = sizeof rbsp };
		if (i == 0) write_sps(&b, p); else write_pps(&b, p);
		if (b.overflow) return 0;
		len = append_nal(out, cap, len, 3, i == 0 ? NAL_SPS : NAL_PPS, rbsp, b.pos / 8);
		if (!len) return 0;
	}
	return len;
}

size_t h264_write_slice_header(const struct h264_params *p, bool idr, unsigned frame_num, unsigned idr_pic_id, unsigned ref_distance,
			       unsigned first_mb, uint8_t *out, size_t cap)
{
	uint8_t rbsp[64];
	struct bits b = { .buf = rbsp, .cap = sizeof rbsp };
	if (first_mb >= h264_mb_columns(p) * h264_mb_rows(p)) return 0;
	if (!idr && ref_distance >= (1u << H264_LOG2_MAX_FRAME_NUM)) return 0;

	ue(&b, first_mb);                /* first_mb_in_slice */
	ue(&b, idr ? 7 : 5);             /* slice_type: I or P, every slice of the picture has the same type */
	ue(&b, 0);                       /* pic_parameter_set_id */
	u(&b, H264_LOG2_MAX_FRAME_NUM, frame_num & ((1u << H264_LOG2_MAX_FRAME_NUM) - 1));
	if (idr) ue(&b, idr_pic_id);     /* idr_pic_id (POC type 2: no POC fields) */
	if (!idr) {
		u(&b, 1, 0);             /* num_ref_idx_active_override_flag: one active reference */
		if (ref_distance > 1) {
			u(&b, 1, 1);     /* ref_pic_list_modification_flag_l0 */
			ue(&b, 0);       /* modification_of_pic_nums_idc: subtract from the predicted picture number */
			ue(&b, ref_distance - 1); /* abs_diff_pic_num_minus1: CurrPicNum - PicNum - 1 */
			ue(&b, 3);       /* end of the list */
		} else {
			u(&b, 1, 0);     /* ref_pic_list_modification_flag_l0 */
		}
	}
	if (idr) {
		u(&b, 1, 0);             /* no_output_of_prior_pics_flag */
		u(&b, 1, 0);             /* long_term_reference_flag */
	} else {
		u(&b, 1, 0);             /* adaptive_ref_pic_marking_mode_flag: sliding window */
		ue(&b, 0);               /* cabac_init_idc */
	}
	se(&b, 0);                       /* slice_qp_delta */
	while (b.pos & 7) put_bit(&b, 1); /* cabac_alignment_one_bit */
	if (b.overflow) return 0;

	return append_nal(out, cap, 0, idr ? 3 : 2, idr ? NAL_IDR : NAL_SLICE, rbsp, b.pos / 8);
}
