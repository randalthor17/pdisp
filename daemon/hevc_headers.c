#include "hevc_headers.h"

#include <string.h>

#define NAL_VPS 32
#define NAL_SPS 33
#define NAL_PPS 34

struct bits {
	uint8_t *buf;
	size_t cap;
	size_t pos; /* in bits */
	bool overflow;
};

static void put_bit(struct bits *b, unsigned v)
{
	if (b->pos / 8 >= b->cap) { b->overflow = true; return; }
	if ((b->pos & 7) == 0) b->buf[b->pos / 8] = 0;
	if (v) b->buf[b->pos / 8] |= (uint8_t)(0x80 >> (b->pos & 7));
	b->pos++;
}

static void u(struct bits *b, unsigned n, uint32_t v)
{
	for (unsigned i = n; i-- > 0;) put_bit(b, (v >> i) & 1);
}

static void ue(struct bits *b, uint32_t v)
{
	uint64_t x = (uint64_t)v + 1;
	unsigned len = 0;
	while ((x >> len) > 1) len++;
	for (unsigned i = 0; i < len; i++) put_bit(b, 0);
	for (unsigned i = len + 1; i-- > 0;) put_bit(b, (x >> i) & 1);
}

static void se(struct bits *b, int32_t v)
{
	ue(b, v > 0 ? (uint32_t)(2 * v - 1) : (uint32_t)(-2 * v));
}

static void trailing_bits(struct bits *b)
{
	put_bit(b, 1);
	while (b->pos & 7) put_bit(b, 0);
}

static void profile_tier_level(struct bits *b, unsigned level_idc)
{
	u(b, 2, 0);  /* general_profile_space */
	u(b, 1, 0);  /* general_tier_flag: Main tier */
	u(b, 5, 1);  /* general_profile_idc: Main */
	for (unsigned j = 0; j < 32; j++) u(b, 1, j == 1 || j == 2); /* compatible with Main and Main 10 */
	u(b, 1, 1);  /* general_progressive_source_flag */
	u(b, 1, 0);  /* general_interlaced_source_flag */
	u(b, 1, 0);  /* general_non_packed_constraint_flag */
	u(b, 1, 1);  /* general_frame_only_constraint_flag */
	u(b, 32, 0); /* 43 reserved bits ... */
	u(b, 11, 0);
	u(b, 1, 0);  /* ... plus general_inbld_flag / reserved */
	u(b, 8, level_idc);
}

static void write_vps(struct bits *b, const struct hevc_params *p)
{
	u(b, 4, 0);      /* vps_video_parameter_set_id */
	u(b, 1, 1);      /* vps_base_layer_internal_flag */
	u(b, 1, 1);      /* vps_base_layer_available_flag */
	u(b, 6, 0);      /* vps_max_layers_minus1 */
	u(b, 3, 0);      /* vps_max_sub_layers_minus1 */
	u(b, 1, 1);      /* vps_temporal_id_nesting_flag */
	u(b, 16, 0xffff);
	profile_tier_level(b, p->level_idc);
	u(b, 1, 0);      /* vps_sub_layer_ordering_info_present_flag */
	ue(b, 1);        /* vps_max_dec_pic_buffering_minus1: current + one reference */
	ue(b, 0);        /* vps_max_num_reorder_pics */
	ue(b, 0);        /* vps_max_latency_increase_plus1 */
	u(b, 6, 0);      /* vps_max_layer_id */
	ue(b, 0);        /* vps_num_layer_sets_minus1 */
	u(b, 1, 1);      /* vps_timing_info_present_flag */
	u(b, 32, 1);     /* vps_num_units_in_tick */
	u(b, 32, p->fps);/* vps_time_scale */
	u(b, 1, 0);      /* vps_poc_proportional_to_timing_flag */
	ue(b, 0);        /* vps_num_hrd_parameters */
	u(b, 1, 0);      /* vps_extension_flag */
	trailing_bits(b);
}

static void write_sps(struct bits *b, const struct hevc_params *p)
{
	u(b, 4, 0);  /* sps_video_parameter_set_id */
	u(b, 3, 0);  /* sps_max_sub_layers_minus1 */
	u(b, 1, 1);  /* sps_temporal_id_nesting_flag */
	profile_tier_level(b, p->level_idc);
	ue(b, 0);    /* sps_seq_parameter_set_id */
	ue(b, 1);    /* chroma_format_idc: 4:2:0 */
	ue(b, p->width);
	ue(b, p->height);
	u(b, 1, 0);  /* conformance_window_flag */
	ue(b, 0);    /* bit_depth_luma_minus8 */
	ue(b, 0);    /* bit_depth_chroma_minus8 */
	ue(b, 8);    /* log2_max_pic_order_cnt_lsb_minus4: 12 bits */
	u(b, 1, 0);  /* sps_sub_layer_ordering_info_present_flag */
	ue(b, 1);    /* sps_max_dec_pic_buffering_minus1 */
	ue(b, 0);    /* sps_max_num_reorder_pics */
	ue(b, 0);    /* sps_max_latency_increase_plus1 */
	ue(b, 0);    /* log2_min_luma_coding_block_size_minus3: 8x8 */
	ue(b, 2);    /* log2_diff_max_min_luma_coding_block_size: 32x32 CTU */
	ue(b, 0);    /* log2_min_luma_transform_block_size_minus2: 4x4 */
	ue(b, 3);    /* log2_diff_max_min_luma_transform_block_size: 32x32 */
	ue(b, 3);    /* max_transform_hierarchy_depth_inter */
	ue(b, 3);    /* max_transform_hierarchy_depth_intra */
	u(b, 1, 0);  /* scaling_list_enabled_flag */
	u(b, 1, 1);  /* amp_enabled_flag */
	u(b, 1, 0);  /* sample_adaptive_offset_enabled_flag */
	u(b, 1, 0);  /* pcm_enabled_flag */
	ue(b, 0);    /* num_short_term_ref_pic_sets: each slice codes its own */
	u(b, 1, 0);  /* long_term_ref_pics_present_flag */
	u(b, 1, 0);  /* sps_temporal_mvp_enabled_flag */
	u(b, 1, 0);  /* strong_intra_smoothing_enabled_flag */
	u(b, 1, 1);  /* vui_parameters_present_flag */

	/* VUI */
	u(b, 1, 0);  /* aspect_ratio_info_present_flag */
	u(b, 1, 0);  /* overscan_info_present_flag */
	u(b, 1, 1);  /* video_signal_type_present_flag */
	u(b, 3, 5);  /* video_format: unspecified */
	u(b, 1, p->full_range);
	u(b, 1, 1);  /* colour_description_present_flag */
	u(b, 8, p->colour_primaries);
	u(b, 8, p->transfer_characteristics);
	u(b, 8, p->matrix_coefficients);
	u(b, 1, 0);  /* chroma_loc_info_present_flag */
	u(b, 1, 0);  /* neutral_chroma_indication_flag */
	u(b, 1, 0);  /* field_seq_flag */
	u(b, 1, 0);  /* frame_field_info_present_flag */
	u(b, 1, 0);  /* default_display_window_flag */
	u(b, 1, 1);  /* vui_timing_info_present_flag */
	u(b, 32, 1);
	u(b, 32, p->fps);
	u(b, 1, 0);  /* vui_poc_proportional_to_timing_flag */
	u(b, 1, 0);  /* vui_hrd_parameters_present_flag */
	u(b, 1, 1);  /* bitstream_restriction_flag */
	u(b, 1, 0);  /* tiles_fixed_structure_flag */
	u(b, 1, 1);  /* motion_vectors_over_pic_boundaries_flag */
	u(b, 1, 1);  /* restricted_ref_pic_lists_flag */
	ue(b, 0);    /* min_spatial_segmentation_idc */
	ue(b, 0);    /* max_bytes_per_pic_denom */
	ue(b, 0);    /* max_bits_per_min_cu_denom */
	ue(b, 15);   /* log2_max_mv_length_horizontal */
	ue(b, 15);   /* log2_max_mv_length_vertical */

	u(b, 1, 0);  /* sps_extension_present_flag */
	trailing_bits(b);
}

static void write_pps(struct bits *b, const struct hevc_params *p)
{
	ue(b, 0);    /* pps_pic_parameter_set_id */
	ue(b, 0);    /* pps_seq_parameter_set_id */
	u(b, 1, 0);  /* dependent_slice_segments_enabled_flag */
	u(b, 1, 0);  /* output_flag_present_flag */
	u(b, 3, 0);  /* num_extra_slice_header_bits */
	u(b, 1, 0);  /* sign_data_hiding_enabled_flag */
	u(b, 1, 0);  /* cabac_init_present_flag */
	ue(b, 0);    /* num_ref_idx_l0_default_active_minus1 */
	ue(b, 0);    /* num_ref_idx_l1_default_active_minus1 */
	se(b, p->init_qp - 26);
	u(b, 1, 0);  /* constrained_intra_pred_flag */
	u(b, 1, 0);  /* transform_skip_enabled_flag */
	u(b, 1, 1);  /* cu_qp_delta_enabled_flag */
	ue(b, 0);    /* diff_cu_qp_delta_depth */
	se(b, 0);    /* pps_cb_qp_offset */
	se(b, 0);    /* pps_cr_qp_offset */
	u(b, 1, 0);  /* pps_slice_chroma_qp_offsets_present_flag */
	u(b, 1, 0);  /* weighted_pred_flag */
	u(b, 1, 0);  /* weighted_bipred_flag */
	u(b, 1, 0);  /* transquant_bypass_enabled_flag */
	u(b, 1, 0);  /* tiles_enabled_flag */
	u(b, 1, 0);  /* entropy_coding_sync_enabled_flag */
	u(b, 1, 1);  /* pps_loop_filter_across_slices_enabled_flag */
	u(b, 1, 0);  /* deblocking_filter_control_present_flag */
	u(b, 1, 0);  /* pps_scaling_list_data_present_flag */
	u(b, 1, 0);  /* lists_modification_present_flag */
	ue(b, 0);    /* log2_parallel_merge_level_minus2 */
	u(b, 1, 0);  /* slice_segment_header_extension_present_flag */
	u(b, 1, 0);  /* pps_extension_present_flag */
	trailing_bits(b);
}

/* Appends one NAL unit: start code, 2-byte header, RBSP with emulation prevention. Returns the new length, or 0. */
static size_t append_nal(uint8_t *out, size_t cap, size_t len, unsigned nal_type, const uint8_t *rbsp, size_t rbsp_len)
{
	if (len + 6 > cap) return 0;
	static const uint8_t sc[4] = { 0, 0, 0, 1 };
	memcpy(out + len, sc, 4);
	len += 4;
	out[len++] = (uint8_t)(nal_type << 1);
	out[len++] = 1; /* nuh_layer_id 0, nuh_temporal_id_plus1 1 */
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

size_t hevc_write_slice_header(const struct hevc_params *p, bool idr, unsigned poc, uint8_t *out, size_t cap)
{
	(void)p;
	uint8_t rbsp[64];
	struct bits b = { .buf = rbsp, .cap = sizeof rbsp };

	u(&b, 1, 1);                 /* first_slice_segment_in_pic_flag */
	if (idr) u(&b, 1, 0);        /* no_output_of_prior_pics_flag (IRAP pictures only) */
	ue(&b, 0);                   /* slice_pic_parameter_set_id */
	ue(&b, idr ? 2 : 1);         /* slice_type: I = 2, P = 1 */
	if (!idr) {
		u(&b, 12, poc & 0xfff);  /* slice_pic_order_cnt_lsb (log2_max_pic_order_cnt_lsb = 12) */
		u(&b, 1, 0);             /* short_term_ref_pic_set_sps_flag: the RPS follows */
		ue(&b, 1);               /* num_negative_pics */
		ue(&b, 0);               /* num_positive_pics */
		ue(&b, 0);               /* delta_poc_s0_minus1: the previous picture */
		u(&b, 1, 1);             /* used_by_curr_pic_s0_flag */
		u(&b, 1, 0);             /* num_ref_idx_active_override_flag */
		ue(&b, 0);               /* five_minus_max_num_merge_cand: 5 candidates */
	}
	se(&b, 0);                   /* slice_qp_delta */
	u(&b, 1, 1);                 /* slice_loop_filter_across_slices_enabled_flag */
	trailing_bits(&b);           /* byte_alignment(): a 1 bit, then zeros */
	if (b.overflow) return 0;

	return append_nal(out, cap, 0, idr ? 19 : 1, rbsp, b.pos / 8);
}

size_t hevc_write_parameter_sets(const struct hevc_params *p, uint8_t *out, size_t cap)
{
	if (p->width == 0 || p->height == 0 || p->width % 8 || p->height % 8 || p->fps == 0) return 0;

	static void (*const writers[3])(struct bits *, const struct hevc_params *) = { write_vps, write_sps, write_pps };
	static const unsigned types[3] = { NAL_VPS, NAL_SPS, NAL_PPS };
	size_t len = 0;
	for (int i = 0; i < 3; i++) {
		uint8_t rbsp[256];
		struct bits b = { .buf = rbsp, .cap = sizeof rbsp };
		writers[i](&b, p);
		if (b.overflow) return 0;
		len = append_nal(out, cap, len, types[i], rbsp, b.pos / 8);
		if (!len) return 0;
	}
	return len;
}
