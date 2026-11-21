#include "encoder.h"

#include "hevc_headers.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_enc_hevc.h>
#include <va/va_vpp.h>

#define CTU_SIZE 32
#define NUM_RECON 7 /* reconstruction surfaces: the current picture and up to six retained references */
#define LOG2_MAX_POC_LSB 8
#define HEVC_NAL_TRAIL_R 1
#define HEVC_NAL_IDR_W_RADL 19
#define HEVC_SLICE_P 1
#define HEVC_SLICE_I 2

struct encoder {
	struct encoder_config cfg;
	int drm_fd;
	VADisplay dpy;
	const char *vendor;
	char desc[256];

	VAConfigID enc_cfg, vpp_cfg;
	VAContextID enc_ctx, vpp_ctx;
	VASurfaceID rgb;           /* BGRX upload target */
	VASurfaceID nv12;          /* VPP output, encoder input */
	VASurfaceID recon[NUM_RECON];
	VABufferID coded;
	VAImageFormat rgb_fmt;

	uint64_t frame_count;
	unsigned poc;          /* picture order count since the last IDR */
	unsigned refresh_pos;  /* frames since the last IDR, drives the intra refresh sweep */
	struct {
		bool valid;        /* held by the decoder and not invalidated: usable as a reference */
		uint64_t idx;
		int poc;
	} pics[NUM_RECON];     /* the picture in recon[i] */
	bool force_idr_next;   /* every retained picture was invalidated */
	bool rc_dirty;         /* the bitrate changed: send new rate control parameters with the next picture */

	uint8_t *out;
	size_t out_cap;
};

static double now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

#define VA_OK(call)                                                                                        \
	do {                                                                                               \
		VAStatus s_ = (call);                                                                      \
		if (s_ != VA_STATUS_SUCCESS) {                                                             \
			fprintf(stderr, "encoder: %s failed: %s (%d)\n", #call, vaErrorStr(s_), s_);       \
			return -EIO;                                                                       \
		}                                                                                          \
	} while (0)

/* ---- display and capability selection */

static int try_open_display(struct encoder *e, const char *node)
{
	int fd = open(node, O_RDWR | O_CLOEXEC);
	if (fd < 0) return -errno;
	VADisplay dpy = vaGetDisplayDRM(fd);
	int major, minor;
	if (!vaDisplayIsValid(dpy) || vaInitialize(dpy, &major, &minor) != VA_STATUS_SUCCESS) {
		close(fd);
		return -ENODEV;
	}

	int n = vaMaxNumEntrypoints(dpy);
	VAEntrypoint *eps = malloc(sizeof *eps * n);
	bool ok = false;
	if (eps && vaQueryConfigEntrypoints(dpy, VAProfileHEVCMain, eps, &n) == VA_STATUS_SUCCESS)
		for (int i = 0; i < n; i++)
			if (eps[i] == VAEntrypointEncSlice) ok = true;
	free(eps);
	if (!ok) {
		vaTerminate(dpy);
		close(fd);
		return -ENODEV;
	}

	e->drm_fd = fd;
	e->dpy = dpy;
	e->vendor = vaQueryVendorString(dpy);
	snprintf(e->desc, sizeof e->desc, "%s on %s (libva %d.%d)", e->vendor ? e->vendor : "?", node, major, minor);
	return 0;
}

static int open_display(struct encoder *e)
{
	if (e->cfg.render_node) return try_open_display(e, e->cfg.render_node);
	char path[64];
	for (int i = 128; i < 144; i++) {
		snprintf(path, sizeof path, "/dev/dri/renderD%d", i);
		if (try_open_display(e, path) == 0) return 0;
	}
	return -ENODEV;
}

/* ---- buffers */

static int create_buffer(struct encoder *e, VAContextID ctx, VABufferType type, unsigned size, void *data, VABufferID *id)
{
	VA_OK(vaCreateBuffer(e->dpy, ctx, type, size, 1, data, id));
	return 0;
}

static int create_misc(struct encoder *e, VAEncMiscParameterType type, const void *payload, size_t len, VABufferID *id)
{
	size_t total = sizeof(VAEncMiscParameterBuffer) + len;
	VAEncMiscParameterBuffer *p;
	VA_OK(vaCreateBuffer(e->dpy, e->enc_ctx, VAEncMiscParameterBufferType, total, 1, NULL, id));
	VA_OK(vaMapBuffer(e->dpy, *id, (void **)&p));
	p->type = type;
	memcpy(p->data, payload, len);
	VA_OK(vaUnmapBuffer(e->dpy, *id));
	return 0;
}

static unsigned level_idc(unsigned w, unsigned h, unsigned fps);

static struct hevc_params make_hevc_params(const struct encoder *e)
{
	return (struct hevc_params){ .width = e->cfg.width, .height = e->cfg.height, .fps = e->cfg.fps,
				     .level_idc = level_idc(e->cfg.width, e->cfg.height, e->cfg.fps), .init_qp = 26,
				     .dpb_size = NUM_RECON, .full_range = false, .colour_primaries = 1,
				     .transfer_characteristics = 13, .matrix_coefficients = 1 };
}

size_t encoder_parameter_sets(const struct encoder *e, uint8_t *out, size_t cap)
{
	struct hevc_params hp = make_hevc_params(e);
	return hevc_write_parameter_sets(&hp, out, cap);
}

void encoder_set_bitrate(struct encoder *e, unsigned bitrate_kbps)
{
	if (bitrate_kbps == 0 || bitrate_kbps == e->cfg.bitrate_kbps) return;
	e->cfg.bitrate_kbps = bitrate_kbps;
	e->rc_dirty = true;
}

static unsigned level_idc(unsigned w, unsigned h, unsigned fps)
{
	/* HEVC main tier: MaxLumaSr 267386880 for level 5.0, 534773760 for 5.1, 1069547520 for 5.2 */
	unsigned long long rate = (unsigned long long)w * h * fps;
	if (rate <= 267386880ull) return 150;
	if (rate <= 534773760ull) return 153;
	return 156;
}

/* ---- open / close */

int encoder_open(const struct encoder_config *cfg, struct encoder **out)
{
	if (cfg->width < 64 || cfg->height < 64 || cfg->width % 8 || cfg->height % 8 || cfg->width > 8192 ||
	    cfg->height > 4352 || cfg->fps == 0 || cfg->bitrate_kbps == 0)
		return -EINVAL;

	struct encoder *e = calloc(1, sizeof *e);
	if (!e) return -ENOMEM;
	e->cfg = *cfg;
	e->drm_fd = -1;
	int rc = open_display(e);
	if (rc) {
		free(e);
		return rc;
	}
	const unsigned w = cfg->width, h = cfg->height;

	/* VPP: RGB -> NV12 */
	if (vaCreateConfig(e->dpy, VAProfileNone, VAEntrypointVideoProc, NULL, 0, &e->vpp_cfg) != VA_STATUS_SUCCESS)
		goto fail;

	/* RGB upload surface, BGRX byte order (what evdi delivers) */
	VASurfaceAttrib attr = { .type = VASurfaceAttribPixelFormat, .flags = VA_SURFACE_ATTRIB_SETTABLE,
				 .value = { .type = VAGenericValueTypeInteger, .value = { .i = VA_FOURCC_BGRX } } };
	if (vaCreateSurfaces(e->dpy, VA_RT_FORMAT_RGB32, w, h, &e->rgb, 1, &attr, 1) != VA_STATUS_SUCCESS) goto fail;

	/* the image format used to fill it */
	int nfmt = vaMaxNumImageFormats(e->dpy);
	VAImageFormat *fmts = malloc(sizeof *fmts * nfmt);
	bool found = false;
	if (fmts && vaQueryImageFormats(e->dpy, fmts, &nfmt) == VA_STATUS_SUCCESS)
		for (int i = 0; i < nfmt && !found; i++)
			if (fmts[i].fourcc == VA_FOURCC_BGRX || fmts[i].fourcc == VA_FOURCC_BGRA) {
				e->rgb_fmt = fmts[i];
				found = true;
			}
	free(fmts);
	if (!found) goto fail;

	/* NV12 + reconstruction surfaces */
	if (vaCreateSurfaces(e->dpy, VA_RT_FORMAT_YUV420, w, h, &e->nv12, 1, NULL, 0) != VA_STATUS_SUCCESS) goto fail;
	if (vaCreateSurfaces(e->dpy, VA_RT_FORMAT_YUV420, w, h, e->recon, NUM_RECON, NULL, 0) != VA_STATUS_SUCCESS)
		goto fail;
	if (vaCreateContext(e->dpy, e->vpp_cfg, w, h, VA_PROGRESSIVE, &e->nv12, 1, &e->vpp_ctx) != VA_STATUS_SUCCESS)
		goto fail;

	/* encoder: HEVC Main, CBR, parameter sets generated by the driver (no packed headers) */
	VAConfigAttrib attribs[4] = {
		{ .type = VAConfigAttribRTFormat, .value = VA_RT_FORMAT_YUV420 },
		{ .type = VAConfigAttribRateControl, .value = e->cfg.vbr ? VA_RC_VBR : VA_RC_CBR },
		{ .type = VAConfigAttribEncPackedHeaders, .value = VA_ENC_PACKED_HEADER_SEQUENCE | VA_ENC_PACKED_HEADER_SLICE },
	};
	int nattr = 3;
	if (e->cfg.intra_refresh_frames) {
		attribs[nattr].type = VAConfigAttribEncIntraRefresh;
		attribs[nattr].value = VA_ENC_INTRA_REFRESH_ROLLING_COLUMN;
		nattr++;
	}
	if (vaCreateConfig(e->dpy, VAProfileHEVCMain, VAEntrypointEncSlice, attribs, nattr, &e->enc_cfg) != VA_STATUS_SUCCESS)
		goto fail;
	VASurfaceID targets[1 + NUM_RECON];
	targets[0] = e->nv12;
	for (int i = 0; i < NUM_RECON; i++) targets[1 + i] = e->recon[i];
	if (vaCreateContext(e->dpy, e->enc_cfg, w, h, VA_PROGRESSIVE, targets, 1 + NUM_RECON, &e->enc_ctx) !=
	    VA_STATUS_SUCCESS)
		goto fail;

	unsigned coded_size = w * h; /* generous: far above any CBR frame */
	if (vaCreateBuffer(e->dpy, e->enc_ctx, VAEncCodedBufferType, coded_size, 1, NULL, &e->coded) != VA_STATUS_SUCCESS)
		goto fail;

	e->out_cap = coded_size;
	e->out = malloc(e->out_cap);
	if (!e->out) goto fail;

	*out = e;
	return 0;

fail:
	encoder_close(e);
	return -EIO;
}

const char *encoder_description(const struct encoder *enc)
{
	return enc->desc;
}

void encoder_close(struct encoder *e)
{
	if (!e) return;
	if (e->dpy) {
		if (e->coded) vaDestroyBuffer(e->dpy, e->coded);
		if (e->enc_ctx) vaDestroyContext(e->dpy, e->enc_ctx);
		if (e->vpp_ctx) vaDestroyContext(e->dpy, e->vpp_ctx);
		if (e->enc_cfg) vaDestroyConfig(e->dpy, e->enc_cfg);
		if (e->vpp_cfg) vaDestroyConfig(e->dpy, e->vpp_cfg);
		VASurfaceID s[2 + NUM_RECON];
		int n = 0;
		if (e->rgb) s[n++] = e->rgb;
		if (e->nv12) s[n++] = e->nv12;
		for (int i = 0; i < NUM_RECON; i++)
			if (e->recon[i]) s[n++] = e->recon[i];
		if (n) vaDestroySurfaces(e->dpy, s, n);
		vaTerminate(e->dpy);
	}
	if (e->drm_fd >= 0) close(e->drm_fd);
	free(e->out);
	free(e);
}

/* ---- per-frame work */

static int upload(struct encoder *e, const uint8_t *bgra, int stride)
{
	VAImage img;
	void *dst;
	VA_OK(vaCreateImage(e->dpy, &e->rgb_fmt, e->cfg.width, e->cfg.height, &img));
	VAStatus s = vaMapBuffer(e->dpy, img.buf, &dst);
	if (s != VA_STATUS_SUCCESS) {
		vaDestroyImage(e->dpy, img.image_id);
		fprintf(stderr, "encoder: vaMapBuffer(image) failed: %s\n", vaErrorStr(s));
		return -EIO;
	}
	size_t row = (size_t)e->cfg.width * 4;
	for (unsigned y = 0; y < e->cfg.height; y++)
		memcpy((uint8_t *)dst + img.offsets[0] + (size_t)img.pitches[0] * y, bgra + (size_t)stride * y, row);
	vaUnmapBuffer(e->dpy, img.buf);
	s = vaPutImage(e->dpy, e->rgb, img.image_id, 0, 0, e->cfg.width, e->cfg.height, 0, 0, e->cfg.width, e->cfg.height);
	vaDestroyImage(e->dpy, img.image_id);
	if (s != VA_STATUS_SUCCESS) {
		fprintf(stderr, "encoder: vaPutImage failed: %s\n", vaErrorStr(s));
		return -EIO;
	}
	return 0;
}

static int convert(struct encoder *e)
{
	VAProcPipelineParameterBuffer pp;
	memset(&pp, 0, sizeof pp);
	pp.surface = e->rgb;
	pp.output_background_color = 0xff000000;
	pp.surface_color_standard = VAProcColorStandardNone;
	pp.output_color_standard = VAProcColorStandardBT709;
	pp.input_color_properties.color_range = VA_SOURCE_RANGE_FULL;
	pp.input_color_properties.colour_primaries = 1;
	pp.input_color_properties.transfer_characteristics = 13; /* sRGB */
	pp.input_color_properties.matrix_coefficients = 0;       /* RGB */
	pp.output_color_properties.color_range = VA_SOURCE_RANGE_REDUCED;
	pp.output_color_properties.colour_primaries = 1;
	pp.output_color_properties.transfer_characteristics = 13;
	pp.output_color_properties.matrix_coefficients = 1; /* BT.709 */

	VABufferID buf;
	VA_OK(vaCreateBuffer(e->dpy, e->vpp_ctx, VAProcPipelineParameterBufferType, sizeof pp, 1, &pp, &buf));
	VAStatus s = vaBeginPicture(e->dpy, e->vpp_ctx, e->nv12);
	if (s == VA_STATUS_SUCCESS) s = vaRenderPicture(e->dpy, e->vpp_ctx, &buf, 1);
	if (s == VA_STATUS_SUCCESS) s = vaEndPicture(e->dpy, e->vpp_ctx);
	vaDestroyBuffer(e->dpy, buf);
	if (s != VA_STATUS_SUCCESS) {
		fprintf(stderr, "encoder: colour conversion failed: %s\n", vaErrorStr(s));
		return -EIO;
	}
	VA_OK(vaSyncSurface(e->dpy, e->nv12));
	return 0;
}

/* the retained pictures, closest first; refs[0] is the one the current frame predicts from */
struct ref_list {
	int n;
	int slot[NUM_RECON];
	int poc[NUM_RECON];
};

static int encode_picture(struct encoder *e, bool idr, int cur_slot, const struct ref_list *rl)
{
	const unsigned w = e->cfg.width, h = e->cfg.height;
	const unsigned ctu_cols = (w + CTU_SIZE - 1) / CTU_SIZE, ctu_rows = (h + CTU_SIZE - 1) / CTU_SIZE;
	const unsigned nslices = e->cfg.slices ? (e->cfg.slices > ctu_rows ? ctu_rows : e->cfg.slices) : 1;
	VABufferID bufs[16 + 2 * 64];
	int nb = 0;

	if (idr) {
		/* a new coded video sequence: sequence parameters and rate control are (re)sent with the IDR */
		VAEncSequenceParameterBufferHEVC seq;
		memset(&seq, 0, sizeof seq);
		seq.general_profile_idc = 1; /* Main */
		seq.general_level_idc = level_idc(w, h, e->cfg.fps);
		seq.general_tier_flag = 0;
		seq.intra_period = 0;
		seq.intra_idr_period = 0;
		seq.ip_period = 1; /* no B-frames */
		seq.bits_per_second = e->cfg.bitrate_kbps * 1000;
		seq.pic_width_in_luma_samples = w;
		seq.pic_height_in_luma_samples = h;
		seq.seq_fields.bits.chroma_format_idc = 1;
		seq.seq_fields.bits.amp_enabled_flag = 1;
		seq.log2_min_luma_coding_block_size_minus3 = 0;      /* 8x8 */
		seq.log2_diff_max_min_luma_coding_block_size = 2;    /* 32x32 CTU */
		seq.log2_min_transform_block_size_minus2 = 0;
		seq.log2_diff_max_min_transform_block_size = 3;
		seq.max_transform_hierarchy_depth_inter = 3;
		seq.max_transform_hierarchy_depth_intra = 3;
		if (create_buffer(e, e->enc_ctx, VAEncSequenceParameterBufferType, sizeof seq, &seq, &bufs[nb++])) return -EIO;

		VAEncMiscParameterFrameRate fr;
		memset(&fr, 0, sizeof fr);
		fr.framerate = e->cfg.fps;
		if (create_misc(e, VAEncMiscParameterTypeFrameRate, &fr, sizeof fr, &bufs[nb++])) return -EIO;

		/* VPS, SPS and PPS: the driver does not write them, so they travel as a packed sequence header */
		struct hevc_params hp = make_hevc_params(e);
		uint8_t ps[512];
		size_t ps_len = hevc_write_parameter_sets(&hp, ps, sizeof ps);
		if (!ps_len) return -EINVAL;
		VAEncPackedHeaderParameterBuffer ph = { .type = VAEncPackedHeaderSequence, .bit_length = (uint32_t)(ps_len * 8),
							.has_emulation_bytes = 1 };
		if (create_buffer(e, e->enc_ctx, VAEncPackedHeaderParameterBufferType, sizeof ph, &ph, &bufs[nb++])) return -EIO;
		if (create_buffer(e, e->enc_ctx, VAEncPackedHeaderDataBufferType, (unsigned)ps_len, ps, &bufs[nb++])) return -EIO;
	}

	if (idr || e->rc_dirty) {
		/* rate control and the HRD buffer: with every IDR, and whenever the target bitrate changes */
		VAEncMiscParameterRateControl rc;
		memset(&rc, 0, sizeof rc);
		rc.bits_per_second = e->cfg.bitrate_kbps * 1000;
		rc.target_percentage = e->cfg.vbr ? (e->cfg.target_percent ? e->cfg.target_percent : 50) : 100;
		rc.window_size = e->cfg.vbv_ms;
		rc.initial_qp = 26;
		rc.min_qp = 10;
		if (create_misc(e, VAEncMiscParameterTypeRateControl, &rc, sizeof rc, &bufs[nb++])) return -EIO;

		VAEncMiscParameterHRD hrd;
		memset(&hrd, 0, sizeof hrd);
		hrd.buffer_size = (uint64_t)e->cfg.bitrate_kbps * e->cfg.vbv_ms;
		hrd.initial_buffer_fullness = hrd.buffer_size / 2;
		if (create_misc(e, VAEncMiscParameterTypeHRD, &hrd, sizeof hrd, &bufs[nb++])) return -EIO;

		e->rc_dirty = false;
	}

	VAPictureHEVC invalid = { .picture_id = VA_INVALID_ID, .flags = VA_PICTURE_HEVC_INVALID };

	VAEncPictureParameterBufferHEVC pic;
	memset(&pic, 0, sizeof pic);
	pic.decoded_curr_pic = (VAPictureHEVC){ .picture_id = e->recon[cur_slot], .pic_order_cnt = (int32_t)e->poc };
	for (int i = 0; i < 15; i++) pic.reference_frames[i] = invalid;
	if (!idr)
		for (int i = 0; i < rl->n; i++)
			pic.reference_frames[i] = (VAPictureHEVC){ .picture_id = e->recon[rl->slot[i]],
								   .pic_order_cnt = rl->poc[i],
								   .flags = i == 0 ? VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE : 0 };
	pic.coded_buf = e->coded;
	pic.collocated_ref_pic_index = 0xff;
	pic.pic_init_qp = 26;
	pic.diff_cu_qp_delta_depth = 0;
	pic.pic_fields.bits.cu_qp_delta_enabled_flag = 1;
	pic.slice_pic_parameter_set_id = 0;
	pic.nal_unit_type = idr ? HEVC_NAL_IDR_W_RADL : HEVC_NAL_TRAIL_R;
	pic.pic_fields.bits.idr_pic_flag = idr;
	pic.pic_fields.bits.coding_type = idr ? 1 : 2; /* 1 = I, 2 = P */
	pic.pic_fields.bits.reference_pic_flag = 1;
	pic.pic_fields.bits.pps_loop_filter_across_slices_enabled_flag = 1;
	if (create_buffer(e, e->enc_ctx, VAEncPictureParameterBufferType, sizeof pic, &pic, &bufs[nb++])) return -EIO;

	if (!idr && e->cfg.intra_refresh_frames) {
		/* rolling column refresh: this frame intra-codes `size` CTU columns starting at `offset` */
		unsigned size = (ctu_cols + e->cfg.intra_refresh_frames - 1) / e->cfg.intra_refresh_frames;
		unsigned cycle = (ctu_cols + size - 1) / size;
		VAEncMiscParameterRIR rir;
		memset(&rir, 0, sizeof rir);
		rir.rir_flags.value = VA_ENC_INTRA_REFRESH_ROLLING_COLUMN;
		rir.intra_insertion_location = (uint16_t)((e->refresh_pos % cycle) * size);
		rir.intra_insert_size = (uint16_t)size;
		if (create_misc(e, VAEncMiscParameterTypeRIR, &rir, sizeof rir, &bufs[nb++])) return -EIO;
	}

	struct hevc_params hp_slice = { .width = w, .height = h, .fps = e->cfg.fps };
	for (unsigned si = 0; si < nslices; si++) {
	/* equal runs of CTU rows; the first (rows % nslices) slices get one extra row */
	unsigned rows = ctu_rows / nslices + (si < ctu_rows % nslices ? 1 : 0);
	unsigned first_row = si * (ctu_rows / nslices) + (si < ctu_rows % nslices ? si : ctu_rows % nslices);
	unsigned addr = first_row * ctu_cols;

	VAEncSliceParameterBufferHEVC sl;
	memset(&sl, 0, sizeof sl);
	sl.slice_segment_address = addr;
	sl.num_ctu_in_slice = rows * ctu_cols;
	sl.slice_type = idr ? HEVC_SLICE_I : HEVC_SLICE_P;
	sl.slice_pic_parameter_set_id = 0;
	sl.max_num_merge_cand = 5;
	for (int i = 0; i < 15; i++) {
		sl.ref_pic_list0[i] = invalid;
		sl.ref_pic_list1[i] = invalid;
	}
	if (!idr) sl.ref_pic_list0[0] = pic.reference_frames[0];
	sl.slice_fields.bits.last_slice_of_pic_flag = si == nslices - 1;
	sl.slice_fields.bits.slice_loop_filter_across_slices_enabled_flag = 1;
	sl.slice_fields.bits.collocated_from_l0_flag = 1;
	if (create_buffer(e, e->enc_ctx, VAEncSliceParameterBufferType, sizeof sl, &sl, &bufs[nb++])) return -EIO;

	/* the driver also needs the slice header (it parses it, then lets the hardware write the real one) */
	uint8_t sh[128];
	struct hevc_ref rps[NUM_RECON];
	for (int i = 0; !idr && i < rl->n; i++) rps[i] = (struct hevc_ref){ .poc = rl->poc[i], .used = i == 0 };
	size_t sh_len = hevc_write_slice_header(&hp_slice, idr, (int)e->poc, addr, rps, idr ? 0 : rl->n, sh, sizeof sh);
	if (!sh_len) return -EINVAL;
	VAEncPackedHeaderParameterBuffer sph = { .type = VAEncPackedHeaderHEVC_Slice, .bit_length = (uint32_t)(sh_len * 8),
						 .has_emulation_bytes = 1 };
	if (create_buffer(e, e->enc_ctx, VAEncPackedHeaderParameterBufferType, sizeof sph, &sph, &bufs[nb++])) return -EIO;
	if (create_buffer(e, e->enc_ctx, VAEncPackedHeaderDataBufferType, (unsigned)sh_len, sh, &bufs[nb++])) return -EIO;
	}

	VAStatus s = vaBeginPicture(e->dpy, e->enc_ctx, e->nv12);
	if (s == VA_STATUS_SUCCESS) s = vaRenderPicture(e->dpy, e->enc_ctx, bufs, nb);
	if (s == VA_STATUS_SUCCESS) s = vaEndPicture(e->dpy, e->enc_ctx);
	for (int i = 0; i < nb; i++) vaDestroyBuffer(e->dpy, bufs[i]);
	if (s != VA_STATUS_SUCCESS) {
		fprintf(stderr, "encoder: encode submit failed: %s\n", vaErrorStr(s));
		return -EIO;
	}
	VA_OK(vaSyncSurface(e->dpy, e->nv12));
	return 0;
}

static int fetch_coded(struct encoder *e, size_t *size)
{
	VACodedBufferSegment *seg;
	VA_OK(vaMapBuffer(e->dpy, e->coded, (void **)&seg));
	size_t n = 0;
	for (; seg; seg = seg->next) {
		if (n + seg->size > e->out_cap) {
			vaUnmapBuffer(e->dpy, e->coded);
			return -ENOSPC;
		}
		memcpy(e->out + n, seg->buf, seg->size);
		n += seg->size;
	}
	vaUnmapBuffer(e->dpy, e->coded);
	*size = n;
	return 0;
}

void encoder_invalidate_from(struct encoder *e, uint64_t first_lost)
{
	for (int i = 0; i < NUM_RECON; i++)
		if (e->pics[i].valid && e->pics[i].idx >= first_lost) e->pics[i].valid = false;
	bool any = false;
	for (int i = 0; i < NUM_RECON; i++) any |= e->pics[i].valid;
	if (!any) e->force_idr_next = true;
}

void encoder_invalidate_frame(struct encoder *e, uint64_t lost)
{
	for (int i = 0; i < NUM_RECON; i++)
		if (e->pics[i].valid && e->pics[i].idx == lost) e->pics[i].valid = false;
	bool any = false;
	for (int i = 0; i < NUM_RECON; i++) any |= e->pics[i].valid;
	if (!any) e->force_idr_next = true;
}

static void build_ref_list(const struct encoder *e, int cur_slot, struct ref_list *rl)
{
	rl->n = 0;
	for (int i = 0; i < NUM_RECON; i++) {
		if (i == cur_slot || !e->pics[i].valid) continue;
		/* insertion sort by decreasing POC: closest first */
		int j = rl->n++;
		while (j > 0 && rl->poc[j - 1] < e->pics[i].poc) {
			rl->poc[j] = rl->poc[j - 1];
			rl->slot[j] = rl->slot[j - 1];
			j--;
		}
		rl->poc[j] = e->pics[i].poc;
		rl->slot[j] = i;
	}
}

int encoder_encode(struct encoder *e, const uint8_t *bgra, int stride, bool force_idr, struct encoder_packet *out)
{
	bool idr = force_idr || e->frame_count == 0 || e->force_idr_next;
	double t0 = now_ms();
	int rc = upload(e, bgra, stride);
	if (rc) return rc;
	double t1 = now_ms();
	rc = convert(e);
	if (rc) return rc;
	double t2 = now_ms();

	/* the current picture overwrites the oldest slot, which may have been the last valid reference */
	int cur_slot = (int)(e->frame_count % NUM_RECON);
	e->pics[cur_slot].valid = false;
	struct ref_list rl;
	build_ref_list(e, cur_slot, &rl);
	if (!idr && rl.n == 0) idr = true;
	if (idr) {
		e->poc = 0;
		e->refresh_pos = 0;
		e->force_idr_next = false;
		for (int i = 0; i < NUM_RECON; i++) e->pics[i].valid = false; /* an IDR flushes the decoder's buffer */
		rl.n = 0;
	}
	rc = encode_picture(e, idr, cur_slot, &rl);
	if (rc) return rc;
	double t3 = now_ms();

	size_t size = 0;
	rc = fetch_coded(e, &size);
	if (rc) return rc;

	e->pics[cur_slot].valid = true;
	e->pics[cur_slot].idx = e->frame_count;
	e->pics[cur_slot].poc = (int)e->poc;

	out->data = e->out;
	out->size = size;
	out->idr = idr;
	out->frame_index = e->frame_count;
	out->ref_distance = idr ? 0 : (int)(e->poc - (unsigned)rl.poc[0]);
	out->upload_ms = t1 - t0;
	out->convert_ms = t2 - t1;
	out->encode_ms = t3 - t2;

	e->frame_count++;
	e->poc++;
	e->refresh_pos++;
	return 0;
}
