#include "proto.h"

#include <errno.h>
#include <string.h>

/* ---- bounds-checked writer and reader */

struct wr {
	uint8_t *p;
	size_t cap, len;
	int err;
};

static void wput(struct wr *w, const void *src, size_t n)
{
	if (w->err) return;
	if (n > w->cap - w->len) { w->err = -EMSGSIZE; return; }
	if (n) memcpy(w->p + w->len, src, n);
	w->len += n;
}

static void w8(struct wr *w, uint8_t v) { wput(w, &v, 1); }
static void w16(struct wr *w, uint16_t v) { uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) }; wput(w, b, 2); }
static void w32(struct wr *w, uint32_t v)
{
	uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
	wput(w, b, 4);
}
static void w64(struct wr *w, uint64_t v)
{
	uint8_t b[8];
	for (int i = 0; i < 8; i++) b[i] = (uint8_t)(v >> (8 * i));
	wput(w, b, 8);
}
static void wstr(struct wr *w, const char *s, size_t max)
{
	size_t n = strnlen(s, max);
	if (n >= max) { if (!w->err) w->err = -EINVAL; return; } /* unterminated */
	w16(w, (uint16_t)n);
	wput(w, s, n);
}
static int wfinish(const struct wr *w) { return w->err ? w->err : (int)w->len; }

struct rd {
	const uint8_t *p;
	size_t len, pos;
	int err;
};

static int rneed(struct rd *r, size_t n)
{
	if (r->err) return 0;
	if (n > r->len - r->pos) { r->err = -EINVAL; return 0; }
	return 1;
}
static uint8_t r8(struct rd *r) { return rneed(r, 1) ? r->p[r->pos++] : 0; }
static uint16_t r16(struct rd *r)
{
	if (!rneed(r, 2)) return 0;
	uint16_t v = (uint16_t)(r->p[r->pos] | r->p[r->pos + 1] << 8);
	r->pos += 2;
	return v;
}
static uint32_t r32(struct rd *r)
{
	if (!rneed(r, 4)) return 0;
	uint32_t v = 0;
	for (int i = 3; i >= 0; i--) v = v << 8 | r->p[r->pos + i];
	r->pos += 4;
	return v;
}
static uint64_t r64(struct rd *r)
{
	if (!rneed(r, 8)) return 0;
	uint64_t v = 0;
	for (int i = 7; i >= 0; i--) v = v << 8 | r->p[r->pos + i];
	r->pos += 8;
	return v;
}
static void rbytes(struct rd *r, void *dst, size_t n)
{
	if (!rneed(r, n)) return;
	if (n) memcpy(dst, r->p + r->pos, n);
	r->pos += n;
}
static void rstr(struct rd *r, char *dst, size_t cap)
{
	uint16_t n = r16(r);
	if (r->err) return;
	if (n >= cap) { r->err = -EMSGSIZE; return; }
	rbytes(r, dst, n);
	if (!r->err) dst[n] = '\0';
}
static int rfinish(const struct rd *r) { return r->err; }

/* Skips extension TLVs (u16 type, u16 length, value) up to the end of the message. */
static void skip_tlvs(struct rd *r)
{
	while (!r->err && r->pos < r->len) {
		r16(r);
		uint16_t n = r16(r);
		if (!rneed(r, n)) return;
		r->pos += n;
	}
}

/* ---- control framing */

int proto_ctl_frame_write(uint16_t type, const uint8_t *payload, size_t len, uint8_t *out, size_t cap)
{
	if (len > PROTO_MAX_CTL_PAYLOAD) return -EMSGSIZE;
	struct wr w = { out, cap, 0, 0 };
	w32(&w, (uint32_t)len + 4);
	w16(&w, type);
	w16(&w, 0);
	wput(&w, payload, len);
	return wfinish(&w);
}

int proto_ctl_frame_parse(const uint8_t *buf, size_t len, uint16_t *type, const uint8_t **payload, size_t *payload_len)
{
	if (len < 4) return 0;
	struct rd r = { buf, len, 0, 0 };
	uint32_t length = r32(&r);
	if (length < 4 || length - 4 > PROTO_MAX_CTL_PAYLOAD) return -EINVAL;
	if (len - 4 < length) return 0; /* the rest has not arrived */
	*type = r16(&r);
	r16(&r); /* flags: reserved */
	*payload = buf + 8;
	*payload_len = length - 4;
	return (int)(4 + length);
}

/* ---- control messages */

int proto_hello_encode(const struct proto_hello *m, uint8_t *out, size_t cap)
{
	if (m->n_refresh > PROTO_MAX_RATES || m->n_codecs > PROTO_MAX_CODECS || m->orientation > 3) return -EINVAL;
	struct wr w = { out, cap, 0, 0 };
	w16(&w, m->proto_min);
	w16(&w, m->proto_max);
	wstr(&w, m->device_name, PROTO_NAME_MAX);
	w32(&w, m->device_caps);
	w16(&w, m->native_width);
	w16(&w, m->native_height);
	w16(&w, m->density_dpi);
	w8(&w, m->orientation);
	w16(&w, m->inset_left);
	w16(&w, m->inset_top);
	w16(&w, m->inset_right);
	w16(&w, m->inset_bottom);
	w16(&w, m->n_refresh);
	for (int i = 0; i < m->n_refresh; i++) w32(&w, m->refresh_mhz[i]);
	w16(&w, m->n_codecs);
	for (int i = 0; i < m->n_codecs; i++) w8(&w, m->codecs[i]);
	w8(&w, m->max_touch_points);
	w16(&w, m->max_udp_payload);
	wstr(&w, m->app_version, PROTO_VERSION_STR_MAX);
	return wfinish(&w);
}

int proto_hello_decode(const uint8_t *buf, size_t len, struct proto_hello *m)
{
	struct rd r = { buf, len, 0, 0 };
	memset(m, 0, sizeof *m);
	m->proto_min = r16(&r);
	m->proto_max = r16(&r);
	rstr(&r, m->device_name, PROTO_NAME_MAX);
	m->device_caps = r32(&r);
	m->native_width = r16(&r);
	m->native_height = r16(&r);
	m->density_dpi = r16(&r);
	m->orientation = r8(&r);
	m->inset_left = r16(&r);
	m->inset_top = r16(&r);
	m->inset_right = r16(&r);
	m->inset_bottom = r16(&r);
	uint16_t nr = r16(&r);
	if (!r.err && nr > PROTO_MAX_RATES) r.err = -EMSGSIZE;
	for (int i = 0; i < nr && !r.err; i++) m->refresh_mhz[i] = r32(&r);
	m->n_refresh = (uint8_t)nr;
	uint16_t nc = r16(&r);
	if (!r.err && nc > PROTO_MAX_CODECS) r.err = -EMSGSIZE;
	for (int i = 0; i < nc && !r.err; i++) m->codecs[i] = r8(&r);
	m->n_codecs = (uint8_t)nc;
	m->max_touch_points = r8(&r);
	m->max_udp_payload = r16(&r);
	rstr(&r, m->app_version, PROTO_VERSION_STR_MAX);
	if (!r.err && m->orientation > 3) r.err = -EINVAL;
	skip_tlvs(&r);
	return rfinish(&r);
}

static void params_write(struct wr *w, const struct proto_stream_params *m)
{
	w8(w, m->epoch);
	w8(w, m->security);
	w16(w, m->width);
	w16(w, m->height);
	w32(w, m->refresh_mhz);
	w8(w, m->codec);
	w8(w, m->color_range);
	w8(w, m->color_primaries);
	w8(w, m->color_transfer);
	w8(w, m->color_matrix);
	w16(w, m->shard_size);
	w8(w, m->fec_percent);
	w32(w, m->bitrate_kbps);
	w32(w, m->min_bitrate_kbps);
	w32(w, m->max_bitrate_kbps);
	w8(w, m->dpb_frames);
	w16(w, m->param_sets_len);
	wput(w, m->param_sets, m->param_sets_len);
}

static void params_read(struct rd *r, struct proto_stream_params *m)
{
	m->epoch = r8(r);
	m->security = r8(r);
	m->width = r16(r);
	m->height = r16(r);
	m->refresh_mhz = r32(r);
	m->codec = r8(r);
	m->color_range = r8(r);
	m->color_primaries = r8(r);
	m->color_transfer = r8(r);
	m->color_matrix = r8(r);
	m->shard_size = r16(r);
	m->fec_percent = r8(r);
	m->bitrate_kbps = r32(r);
	m->min_bitrate_kbps = r32(r);
	m->max_bitrate_kbps = r32(r);
	m->dpb_frames = r8(r);
	m->param_sets_len = r16(r);
	if (!r->err && m->param_sets_len > PROTO_MAX_PARAM_SETS) r->err = -EMSGSIZE;
	if (!r->err) rbytes(r, m->param_sets, m->param_sets_len);
}

int proto_stream_params_encode(const struct proto_stream_params *m, uint8_t *out, size_t cap)
{
	if (m->param_sets_len > PROTO_MAX_PARAM_SETS) return -EINVAL;
	struct wr w = { out, cap, 0, 0 };
	params_write(&w, m);
	return wfinish(&w);
}

int proto_stream_params_decode(const uint8_t *buf, size_t len, struct proto_stream_params *m)
{
	struct rd r = { buf, len, 0, 0 };
	memset(m, 0, sizeof *m);
	params_read(&r, m);
	skip_tlvs(&r);
	return rfinish(&r);
}

int proto_setup_encode(const struct proto_setup *m, uint8_t *out, size_t cap)
{
	if (m->params.param_sets_len > PROTO_MAX_PARAM_SETS) return -EINVAL;
	struct wr w = { out, cap, 0, 0 };
	w16(&w, m->proto_version);
	w32(&w, m->session_id);
	w16(&w, m->udp_port);
	params_write(&w, &m->params);
	return wfinish(&w);
}

int proto_setup_decode(const uint8_t *buf, size_t len, struct proto_setup *m)
{
	struct rd r = { buf, len, 0, 0 };
	memset(m, 0, sizeof *m);
	m->proto_version = r16(&r);
	m->session_id = r32(&r);
	m->udp_port = r16(&r);
	params_read(&r, &m->params);
	skip_tlvs(&r);
	return rfinish(&r);
}

int proto_reject_encode(const struct proto_reject *m, uint8_t *out, size_t cap)
{
	struct wr w = { out, cap, 0, 0 };
	w16(&w, m->reason);
	wstr(&w, m->text, PROTO_TEXT_MAX);
	return wfinish(&w);
}

int proto_reject_decode(const uint8_t *buf, size_t len, struct proto_reject *m)
{
	struct rd r = { buf, len, 0, 0 };
	memset(m, 0, sizeof *m);
	m->reason = r16(&r);
	rstr(&r, m->text, PROTO_TEXT_MAX);
	skip_tlvs(&r);
	return rfinish(&r);
}

int proto_close_encode(const struct proto_close *m, uint8_t *out, size_t cap)
{
	struct wr w = { out, cap, 0, 0 };
	w16(&w, m->reason);
	wstr(&w, m->text, PROTO_TEXT_MAX);
	return wfinish(&w);
}

int proto_close_decode(const uint8_t *buf, size_t len, struct proto_close *m)
{
	struct rd r = { buf, len, 0, 0 };
	memset(m, 0, sizeof *m);
	m->reason = r16(&r);
	rstr(&r, m->text, PROTO_TEXT_MAX);
	skip_tlvs(&r);
	return rfinish(&r);
}

int proto_cursor_shape_encode(const struct proto_cursor_shape *m, uint8_t *out, size_t cap)
{
	if (m->width == 0 || m->height == 0 || m->width > PROTO_MAX_CURSOR_DIM || m->height > PROTO_MAX_CURSOR_DIM || !m->argb)
		return -EINVAL;
	struct wr w = { out, cap, 0, 0 };
	w16(&w, m->shape_id);
	w16(&w, (uint16_t)m->hot_x);
	w16(&w, (uint16_t)m->hot_y);
	w16(&w, m->width);
	w16(&w, m->height);
	wput(&w, m->argb, (size_t)m->width * m->height * 4);
	return wfinish(&w);
}

int proto_cursor_shape_decode(const uint8_t *buf, size_t len, struct proto_cursor_shape *m)
{
	struct rd r = { buf, len, 0, 0 };
	memset(m, 0, sizeof *m);
	m->shape_id = r16(&r);
	m->hot_x = (int16_t)r16(&r);
	m->hot_y = (int16_t)r16(&r);
	m->width = r16(&r);
	m->height = r16(&r);
	if (r.err) return r.err;
	if (m->width == 0 || m->height == 0 || m->width > PROTO_MAX_CURSOR_DIM || m->height > PROTO_MAX_CURSOR_DIM)
		return -EINVAL;
	size_t n = (size_t)m->width * m->height * 4;
	if (!rneed(&r, n)) return r.err;
	m->argb = buf + r.pos;
	r.pos += n;
	skip_tlvs(&r);
	return rfinish(&r);
}

int proto_token_encode(uint64_t token, uint8_t *out, size_t cap)
{
	struct wr w = { out, cap, 0, 0 };
	w64(&w, token);
	return wfinish(&w);
}

int proto_token_decode(const uint8_t *buf, size_t len, uint64_t *token)
{
	struct rd r = { buf, len, 0, 0 };
	*token = r64(&r);
	return rfinish(&r);
}

/* ---- UDP */

int proto_pkt_hdr_write(const struct proto_pkt_hdr *h, uint8_t *out, size_t cap)
{
	struct wr w = { out, cap, 0, 0 };
	w32(&w, h->session_id);
	w8(&w, h->type);
	w8(&w, h->flags);
	w16(&w, 0);
	w64(&w, h->seq);
	return wfinish(&w);
}

int proto_pkt_hdr_read(const uint8_t *buf, size_t len, struct proto_pkt_hdr *h)
{
	struct rd r = { buf, len, 0, 0 };
	h->session_id = r32(&r);
	h->type = r8(&r);
	h->flags = r8(&r);
	r16(&r); /* reserved */
	h->seq = r64(&r);
	return rfinish(&r);
}

int proto_video_hdr_write(const struct proto_video_hdr *h, uint8_t *out, size_t cap)
{
	if (h->data_shards == 0 || (unsigned)h->data_shards + h->parity_shards > 255 || h->block_count == 0 ||
	    h->block_index >= h->block_count)
		return -EINVAL;
	struct wr w = { out, cap, 0, 0 };
	w32(&w, h->frame_id);
	w32(&w, h->ref_frame_id);
	w64(&w, h->capture_us);
	w32(&w, h->frame_bytes);
	w8(&w, h->epoch);
	w8(&w, h->block_index);
	w8(&w, h->block_count);
	w8(&w, h->shard_index);
	w8(&w, h->data_shards);
	w8(&w, h->parity_shards);
	w8(&w, h->vflags);
	w8(&w, 0);
	return wfinish(&w);
}

int proto_video_hdr_read(const uint8_t *buf, size_t len, struct proto_video_hdr *h)
{
	struct rd r = { buf, len, 0, 0 };
	h->frame_id = r32(&r);
	h->ref_frame_id = r32(&r);
	h->capture_us = r64(&r);
	h->frame_bytes = r32(&r);
	h->epoch = r8(&r);
	h->block_index = r8(&r);
	h->block_count = r8(&r);
	h->shard_index = r8(&r);
	h->data_shards = r8(&r);
	h->parity_shards = r8(&r);
	h->vflags = r8(&r);
	r8(&r); /* reserved */
	if (r.err) return r.err;
	if (h->data_shards == 0 || (unsigned)h->data_shards + h->parity_shards > 255 || h->block_count == 0 ||
	    h->block_index >= h->block_count || h->shard_index >= 255)
		return -EINVAL;
	return 0;
}

int proto_keepalive_encode(const struct proto_keepalive *m, uint8_t *out, size_t cap)
{
	struct wr w = { out, cap, 0, 0 };
	w32(&w, m->newest_frame_id);
	w64(&w, m->now_us);
	w32(&w, m->input_acked);
	return wfinish(&w);
}

int proto_keepalive_decode(const uint8_t *buf, size_t len, struct proto_keepalive *m)
{
	struct rd r = { buf, len, 0, 0 };
	m->newest_frame_id = r32(&r);
	m->now_us = r64(&r);
	m->input_acked = r32(&r);
	return rfinish(&r);
}

int proto_feedback_encode(const struct proto_feedback *m, uint8_t *out, size_t cap)
{
	if (m->nack_count > PROTO_MAX_NACKS) return -EINVAL;
	struct wr w = { out, cap, 0, 0 };
	w8(&w, m->flags);
	w32(&w, m->newest_frame_id);
	w32(&w, m->held_base);
	w64(&w, m->held_bitmap);
	w8(&w, m->nack_count);
	for (int i = 0; i < m->nack_count; i++) {
		w32(&w, m->nacks[i].frame_id);
		w8(&w, m->nacks[i].block_index);
		w8(&w, m->nacks[i].need);
	}
	w16(&w, m->loss_permille);
	w32(&w, (uint32_t)m->queue_delay_us);
	w32(&w, m->recv_kbps);
	w32(&w, m->jitter_us);
	w16(&w, m->decode_ms_x10);
	return wfinish(&w);
}

int proto_feedback_decode(const uint8_t *buf, size_t len, struct proto_feedback *m)
{
	struct rd r = { buf, len, 0, 0 };
	memset(m, 0, sizeof *m);
	m->flags = r8(&r);
	m->newest_frame_id = r32(&r);
	m->held_base = r32(&r);
	m->held_bitmap = r64(&r);
	m->nack_count = r8(&r);
	if (!r.err && m->nack_count > PROTO_MAX_NACKS) r.err = -EMSGSIZE;
	for (int i = 0; i < m->nack_count && !r.err; i++) {
		m->nacks[i].frame_id = r32(&r);
		m->nacks[i].block_index = r8(&r);
		m->nacks[i].need = r8(&r);
	}
	m->loss_permille = r16(&r);
	m->queue_delay_us = (int32_t)r32(&r);
	m->recv_kbps = r32(&r);
	m->jitter_us = r32(&r);
	m->decode_ms_x10 = r16(&r);
	return rfinish(&r);
}

int proto_input_encode(const struct proto_input *m, uint8_t *out, size_t cap)
{
	if (m->count > PROTO_MAX_INPUT_EVENTS) return -EINVAL;
	struct wr w = { out, cap, 0, 0 };
	w8(&w, m->count);
	for (int i = 0; i < m->count; i++) {
		const struct proto_input_event *e = &m->events[i];
		w32(&w, e->seq);
		w8(&w, e->kind);
		w8(&w, e->pointer_id);
		w8(&w, e->tool);
		w8(&w, e->buttons);
		w16(&w, e->x);
		w16(&w, e->y);
		w16(&w, e->pressure);
		w8(&w, (uint8_t)e->tilt_x);
		w8(&w, (uint8_t)e->tilt_y);
		w32(&w, e->time_us);
	}
	return wfinish(&w);
}

int proto_input_decode(const uint8_t *buf, size_t len, struct proto_input *m)
{
	struct rd r = { buf, len, 0, 0 };
	memset(m, 0, sizeof *m);
	m->count = r8(&r);
	if (!r.err && m->count > PROTO_MAX_INPUT_EVENTS) r.err = -EMSGSIZE;
	for (int i = 0; i < m->count && !r.err; i++) {
		struct proto_input_event *e = &m->events[i];
		e->seq = r32(&r);
		e->kind = r8(&r);
		e->pointer_id = r8(&r);
		e->tool = r8(&r);
		e->buttons = r8(&r);
		e->x = r16(&r);
		e->y = r16(&r);
		e->pressure = r16(&r);
		e->tilt_x = (int8_t)r8(&r);
		e->tilt_y = (int8_t)r8(&r);
		e->time_us = r32(&r);
		if (!r.err && e->kind > PROTO_IN_BUTTON) r.err = -EINVAL;
		if (!r.err && e->tool > PROTO_TOOL_ERASER) r.err = -EINVAL;
	}
	return rfinish(&r);
}

int proto_u32_encode(uint32_t v, uint8_t *out, size_t cap)
{
	struct wr w = { out, cap, 0, 0 };
	w32(&w, v);
	return wfinish(&w);
}

int proto_u32_decode(const uint8_t *buf, size_t len, uint32_t *v)
{
	struct rd r = { buf, len, 0, 0 };
	*v = r32(&r);
	return rfinish(&r);
}

int proto_cursor_pos_encode(const struct proto_cursor_pos *m, uint8_t *out, size_t cap)
{
	struct wr w = { out, cap, 0, 0 };
	w16(&w, m->shape_id);
	w16(&w, m->x);
	w16(&w, m->y);
	w32(&w, m->move_seq);
	return wfinish(&w);
}

int proto_cursor_pos_decode(const uint8_t *buf, size_t len, struct proto_cursor_pos *m)
{
	struct rd r = { buf, len, 0, 0 };
	m->shape_id = r16(&r);
	m->x = r16(&r);
	m->y = r16(&r);
	m->move_seq = r32(&r);
	return rfinish(&r);
}

int proto_time_req_encode(uint64_t t1, uint8_t *out, size_t cap)
{
	return proto_token_encode(t1, out, cap);
}

int proto_time_resp_encode(const struct proto_time_resp *m, uint8_t *out, size_t cap)
{
	struct wr w = { out, cap, 0, 0 };
	w64(&w, m->t1);
	w64(&w, m->t2);
	w64(&w, m->t3);
	return wfinish(&w);
}

int proto_time_resp_decode(const uint8_t *buf, size_t len, struct proto_time_resp *m)
{
	struct rd r = { buf, len, 0, 0 };
	m->t1 = r64(&r);
	m->t2 = r64(&r);
	m->t3 = r64(&r);
	return rfinish(&r);
}
