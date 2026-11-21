#include "vrecv.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "video.h"

#define RING 256
#define OWD_SAMPLES 32

enum { FS_UNKNOWN = 0, FS_PENDING, FS_HELD, FS_LOST };

struct entry {
	bool used;
	uint32_t id;
	uint8_t st;
	bool seen_packet;           /* at least one packet of the frame arrived */
	bool nacked;
	uint64_t first_seen_us, last_nack_us;
	/* a frame that completed but waits for its reference */
	uint8_t *pend;
	size_t pend_size;
	uint32_t ref;
	uint64_t capture_us;
	uint8_t vflags;
};

struct owd {
	uint64_t t_us;
	int64_t owd_us;
};

#define MIN_BUCKETS 20 /* 100 ms buckets: about 2 s of history for the minimum one-way delay */
struct min_bucket {
	uint64_t id;
	int64_t min_owd;
	bool used;
};

struct vrecv {
	struct vrecv_config cfg;
	struct vrecv_callbacks cb;
	struct vrecv_stats st;
	struct vrx *rx;
	struct entry ring[RING];
	uint32_t base, newest;   /* frames up to `base` are resolved; `newest` is the highest id known to exist */
	bool started;
	bool awaiting_idr;       /* nothing can be decoded until an IDR arrives */
	bool decoder_error;      /* report need_idr until one is decoded */
	unsigned pending;
	uint64_t seq_out;

	/* link statistics for feedback */
	uint64_t highest_seq, prev_highest_seq, got_in_interval, bytes_in_interval, last_fb_us;
	bool have_seq;
	struct owd samples[OWD_SAMPLES];
	unsigned n_samples;
	struct min_bucket buckets[MIN_BUCKETS];
};

struct vrecv *vrecv_create(const struct vrecv_config *cfg, const struct vrecv_callbacks *cb)
{
	struct vrecv *vr = calloc(1, sizeof *vr);
	if (!vr) return NULL;
	vr->cfg = *cfg;
	if (cb) vr->cb = *cb;
	if (!vr->cfg.shard_size) vr->cfg.shard_size = 1200;
	if (!vr->cfg.giveup_us) vr->cfg.giveup_us = 100000;
	if (!vr->cfg.nack_min_age_us) vr->cfg.nack_min_age_us = 4000;
	if (!vr->cfg.nack_retry_us) vr->cfg.nack_retry_us = 15000;
	if (!vr->cfg.max_pending) vr->cfg.max_pending = 8;
	vr->rx = vrx_create(vr->cfg.shard_size);
	if (!vr->rx) { free(vr); return NULL; }
	vr->awaiting_idr = true;
	return vr;
}

static void entry_clear(struct entry *e)
{
	free(e->pend);
	memset(e, 0, sizeof *e);
}

void vrecv_destroy(struct vrecv *vr)
{
	if (!vr) return;
	for (int i = 0; i < RING; i++) free(vr->ring[i].pend);
	vrx_destroy(vr->rx);
	free(vr);
}

void vrecv_set_epoch(struct vrecv *vr, uint8_t epoch)
{
	vrx_set_epoch(vr->rx, epoch);
	for (int i = 0; i < RING; i++) entry_clear(&vr->ring[i]);
	vr->started = false;
	vr->awaiting_idr = true;
	vr->pending = 0;
}

const struct vrecv_stats *vrecv_stats(const struct vrecv *vr) { return &vr->st; }

static struct entry *ent(struct vrecv *vr, uint32_t id) { return &vr->ring[id % RING]; }

static struct entry *ensure_entry(struct vrecv *vr, uint32_t id, uint64_t now_us)
{
	struct entry *e = ent(vr, id);
	if (!e->used || e->id != id) {
		entry_clear(e);
		e->used = true;
		e->id = id;
		e->first_seen_us = now_us;
	}
	return e;
}

/* state of any frame id as the receiver currently knows it */
static uint8_t state_of(struct vrecv *vr, uint32_t id)
{
	if ((int32_t)(vr->base - id) >= 0) { /* in the resolved region */
		if ((int32_t)(vr->base - id) >= RING - 8) return FS_LOST;
		struct entry *e = ent(vr, id);
		return e->used && e->id == id ? e->st : FS_LOST;
	}
	if ((int32_t)(id - vr->newest) > 0) return FS_UNKNOWN;
	struct entry *e = ent(vr, id);
	return e->used && e->id == id ? e->st : FS_UNKNOWN;
}

static void advance_base(struct vrecv *vr)
{
	while ((int32_t)(vr->newest - vr->base) > 0) {
		uint8_t s = ent(vr, vr->base + 1)->used && ent(vr, vr->base + 1)->id == vr->base + 1 ? ent(vr, vr->base + 1)->st : FS_UNKNOWN;
		if (s != FS_HELD && s != FS_LOST) break;
		vr->base++;
	}
}

static void mark_lost(struct vrecv *vr, uint32_t id)
{
	struct entry *e = ent(vr, id);
	if (!e->used || e->id != id) return;
	if (e->st == FS_HELD || e->st == FS_LOST) return;
	if (e->st == FS_PENDING) vr->pending--;
	free(e->pend);
	e->pend = NULL;
	e->st = FS_LOST;
	vrx_drop_frame(vr->rx, id);
	vr->st.frames_given_up++;
}

/* Learns that frame `id` exists; frames in between that were never heard of become entries waiting for their packets. */
static void note_frame(struct vrecv *vr, uint32_t id, uint64_t now_us)
{
	if (!vr->started) {
		vr->started = true;
		vr->base = id - 1;
		vr->newest = id - 1;
	}
	if ((int32_t)(id - vr->newest) > 0) {
		if (id - vr->newest >= RING - 8) { /* a huge jump: forget everything and resynchronise */
			for (int i = 0; i < RING; i++) entry_clear(&vr->ring[i]);
			vr->pending = 0;
			vr->base = id - 1;
			vr->newest = id - 1;
		}
		for (uint32_t k = vr->newest + 1; (int32_t)(id - k) > 0; k++) ensure_entry(vr, k, now_us); /* gap */
		vr->newest = id;
	}
	if ((int32_t)(id - vr->base) > 0) ensure_entry(vr, id, now_us);
}

static void resolve_pending(struct vrecv *vr);

static void decode_frame(struct vrecv *vr, uint32_t id, uint32_t ref, uint64_t capture_us, uint8_t vflags,
			 const uint8_t *data, size_t size)
{
	struct entry *e = ent(vr, id);
	bool idr = ref == PROTO_NO_REF;
	if (!idr && vr->awaiting_idr) { mark_lost(vr, id); return; }
	struct vrecv_frame f = { id, ref, capture_us, vflags, data, size };
	int rc = vr->cb.decode ? vr->cb.decode(&f, vr->cb.user) : 0;
	if (rc != 0) {
		vr->st.decoder_errors++;
		vr->decoder_error = true;
		vr->awaiting_idr = true;
		mark_lost(vr, id);
		return;
	}
	if (e->st == FS_PENDING) vr->pending--;
	free(e->pend);
	e->pend = NULL;
	e->st = FS_HELD;
	vr->st.frames_decoded++;
	if (idr) {
		vr->st.idr_decoded++;
		vr->awaiting_idr = false;
		vr->decoder_error = false;
		/* an IDR flushes the decoder: every older frame still unresolved can never be shown */
		for (uint32_t k = vr->base + 1; (int32_t)(id - k) > 0; k++) {
			struct entry *o = ent(vr, k);
			if (o->used && o->id == k && o->st != FS_HELD) mark_lost(vr, k);
		}
	}
	resolve_pending(vr);
}

/* Decode waiting frames whose reference arrived, drop those whose reference was given up. */
static void resolve_pending(struct vrecv *vr)
{
	bool changed = true;
	while (changed) {
		changed = false;
		for (uint32_t k = vr->base + 1; (int32_t)(vr->newest - k) >= 0; k++) {
			struct entry *e = ent(vr, k);
			if (!e->used || e->id != k || e->st != FS_PENDING) continue;
			uint8_t rs = state_of(vr, e->ref);
			if (rs == FS_HELD) {
				uint8_t *p = e->pend;
				size_t n = e->pend_size;
				e->pend = NULL; /* decode_frame frees e->pend only if still set */
				decode_frame(vr, k, e->ref, e->capture_us, e->vflags, p, n);
				free(p);
				changed = true;
			} else if (rs == FS_LOST) {
				mark_lost(vr, k);
				changed = true;
			}
		}
	}
	advance_base(vr);
}

/* One-way delay of a frame's first packet: pure queueing (no pacing spread, no repair wait). */
static void add_owd_sample(struct vrecv *vr, uint64_t now_us, uint64_t capture_us)
{
	if (vr->n_samples < OWD_SAMPLES) vr->n_samples++;
	memmove(vr->samples + 1, vr->samples, (OWD_SAMPLES - 1) * sizeof *vr->samples);
	vr->samples[0] = (struct owd){ now_us, (int64_t)now_us - (int64_t)capture_us };
	struct min_bucket *b = &vr->buckets[(now_us / 100000) % MIN_BUCKETS];
	if (!b->used || b->id != now_us / 100000) *b = (struct min_bucket){ now_us / 100000, vr->samples[0].owd_us, true };
	else if (vr->samples[0].owd_us < b->min_owd) b->min_owd = vr->samples[0].owd_us;
}

static void on_complete(struct vrecv *vr, const struct vrx_frame *f, uint64_t now_us)
{
	(void)now_us;
	struct entry *e = ent(vr, f->frame_id);
	if (!e->used || e->id != f->frame_id || (e->st != FS_UNKNOWN)) return; /* already resolved or forgotten */

	if (f->ref_frame_id == PROTO_NO_REF) {
		decode_frame(vr, f->frame_id, f->ref_frame_id, f->capture_us, f->vflags, f->data, f->size);
		advance_base(vr);
		return;
	}
	uint8_t rs = state_of(vr, f->ref_frame_id);
	if (rs == FS_HELD && !vr->awaiting_idr) {
		decode_frame(vr, f->frame_id, f->ref_frame_id, f->capture_us, f->vflags, f->data, f->size);
	} else if (rs == FS_LOST || vr->awaiting_idr) {
		mark_lost(vr, f->frame_id);
	} else if (vr->pending < vr->cfg.max_pending) {
		e->pend = malloc(f->size);
		if (!e->pend) { mark_lost(vr, f->frame_id); }
		else {
			memcpy(e->pend, f->data, f->size);
			e->pend_size = f->size;
			e->ref = f->ref_frame_id;
			e->capture_us = f->capture_us;
			e->vflags = f->vflags;
			e->st = FS_PENDING;
			vr->pending++;
		}
	} else {
		mark_lost(vr, f->frame_id);
	}
	resolve_pending(vr);
}

int vrecv_on_packet(struct vrecv *vr, const uint8_t *pkt, size_t len, uint64_t now_us)
{
	struct proto_pkt_hdr h;
	if (proto_pkt_hdr_read(pkt, len, &h) != 0 || h.session_id != vr->cfg.session_id) return -EINVAL;

	if (!vr->have_seq || h.seq > vr->highest_seq) vr->highest_seq = h.seq;
	if (!vr->have_seq) { vr->prev_highest_seq = h.seq - 1; vr->last_fb_us = now_us; }
	vr->have_seq = true;
	vr->got_in_interval++;
	vr->bytes_in_interval += len;
	vr->st.packets++;

	if (h.type == PROTO_PKT_KEEPALIVE) {
		struct proto_keepalive ka;
		if (proto_keepalive_decode(pkt + PROTO_PKT_HDR_SIZE, len - PROTO_PKT_HDR_SIZE, &ka) != 0) return -EINVAL;
		if (vr->started) { note_frame(vr, ka.newest_frame_id, now_us); advance_base(vr); }
		return 0;
	}
	if (h.type != PROTO_PKT_VIDEO) return 0;

	struct proto_video_hdr vh;
	if (len < PROTO_PKT_HDR_SIZE + PROTO_VIDEO_HDR_SIZE ||
	    proto_video_hdr_read(pkt + PROTO_PKT_HDR_SIZE, len - PROTO_PKT_HDR_SIZE, &vh) != 0)
		return -EINVAL;
	note_frame(vr, vh.frame_id, now_us);
	if (vh.block_index == 0 && vh.shard_index == 0 && !(h.flags & PROTO_PKT_FLAG_REPAIR)) add_owd_sample(vr, now_us, vh.capture_us);
	struct entry *e = ent(vr, vh.frame_id);
	if (!e->used || e->id != vh.frame_id) return 0; /* older than everything resolved */
	e->seen_packet = true;
	if (e->st != FS_UNKNOWN) return 0; /* decided already: pending, held or lost */

	struct vrx_frame f;
	int rc = vrx_push(vr->rx, pkt, len, now_us, &f);
	if (rc == 1) on_complete(vr, &f, now_us);
	advance_base(vr);
	return rc < 0 ? rc : 0;
}

void vrecv_tick(struct vrecv *vr, uint64_t now_us)
{
	if (!vr->started) return;
	for (uint32_t k = vr->base + 1; (int32_t)(vr->newest - k) >= 0; k++) {
		struct entry *e = ent(vr, k);
		if (!e->used || e->id != k) continue;
		if ((e->st == FS_UNKNOWN || e->st == FS_PENDING) && now_us >= e->first_seen_us + vr->cfg.giveup_us) mark_lost(vr, k);
	}
	resolve_pending(vr);
	advance_base(vr);
}

void vrecv_decoder_error(struct vrecv *vr)
{
	vr->decoder_error = true;
	vr->awaiting_idr = true;
	vr->st.decoder_errors++;
}

int vrecv_feedback_packet(struct vrecv *vr, uint64_t now_us, uint8_t *out, size_t cap)
{
	if (!vr->started) return 0;
	struct proto_feedback fb;
	memset(&fb, 0, sizeof fb);
	fb.flags = vr->decoder_error ? PROTO_FB_NEED_IDR : 0;
	fb.newest_frame_id = vr->newest;
	fb.held_base = vr->base;
	for (uint32_t i = 0; i < 64; i++)
		if (state_of(vr, vr->base - i) == FS_HELD) fb.held_bitmap |= 1ull << i;

	/* repairs: blocks of frames we have partial data for, then frames of which nothing arrived */
	struct proto_nack nk[PROTO_MAX_NACKS];
	int n = vrx_nacks(vr->rx, now_us, vr->cfg.nack_min_age_us, vr->cfg.nack_retry_us, nk, PROTO_MAX_NACKS);
	for (int i = 0; i < n; i++) fb.nacks[fb.nack_count++] = nk[i];
	for (uint32_t k = vr->base + 1; (int32_t)(vr->newest - k) >= 0 && fb.nack_count < PROTO_MAX_NACKS; k++) {
		struct entry *e = ent(vr, k);
		if (!e->used || e->id != k || e->st != FS_UNKNOWN || e->seen_packet) continue;
		if (now_us < e->first_seen_us + vr->cfg.nack_min_age_us) continue;
		if (e->nacked && now_us < e->last_nack_us + vr->cfg.nack_retry_us) continue;
		e->nacked = true;
		e->last_nack_us = now_us;
		fb.nacks[fb.nack_count++] = (struct proto_nack){ k, 255, VIDEO_NEED_ALL };
	}

	/* link statistics over the interval since the last report */
	uint64_t expected = vr->highest_seq - vr->prev_highest_seq;
	if (expected > 0 && vr->got_in_interval < expected)
		fb.loss_permille = (uint16_t)((expected - vr->got_in_interval) * 1000 / expected);
	uint64_t dt = now_us > vr->last_fb_us ? now_us - vr->last_fb_us : 1;
	fb.recv_kbps = (uint32_t)(vr->bytes_in_interval * 8 * 1000 / dt);
	vr->prev_highest_seq = vr->highest_seq;
	vr->got_in_interval = 0;
	vr->bytes_in_interval = 0;
	vr->last_fb_us = now_us;

	if (vr->n_samples >= 9) { /* queueing delay: median of the last 9 one-way delays minus the minimum of the last ~2 s */
		int64_t v[9];
		for (int i = 0; i < 9; i++) v[i] = vr->samples[i].owd_us;
		for (int i = 1; i < 9; i++) { int64_t x = v[i]; int j = i - 1; while (j >= 0 && v[j] > x) { v[j + 1] = v[j]; j--; } v[j + 1] = x; }
		int64_t min_owd = v[4];
		for (int i = 0; i < MIN_BUCKETS; i++)
			if (vr->buckets[i].used && now_us / 100000 - vr->buckets[i].id < MIN_BUCKETS && vr->buckets[i].min_owd < min_owd)
				min_owd = vr->buckets[i].min_owd;
		int64_t q = v[4] - min_owd;
		fb.queue_delay_us = (int32_t)(q > 0 ? q : 0);
	}

	struct proto_pkt_hdr h = { .session_id = vr->cfg.session_id, .type = PROTO_PKT_FEEDBACK, .seq = vr->seq_out++ };
	int a = proto_pkt_hdr_write(&h, out, cap);
	if (a < 0) return 0;
	int b = proto_feedback_encode(&fb, out + a, cap - (size_t)a);
	return b < 0 ? 0 : a + b;
}
