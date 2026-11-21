#include "vsend.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "fec.h"
#include "video.h"

#define VS_RING 64
#define VS_QUEUE 16384

struct vs_frame {
	bool used, held, lost, doomed;
	uint32_t id, ref; /* the frame this one predicts from */
	struct vtx_frame vf;
	uint8_t *data;
	uint64_t first_send_us;
	uint8_t next_repair[255];     /* next unused parity index per block */
	uint64_t last_repair_us[255];
	unsigned repair_budget;       /* shards still allowed as repairs for this frame */
};

struct vs_job {
	uint32_t frame_id;
	uint8_t block, shard, flags;
};

struct vs_queue {
	struct vs_job *jobs;
	unsigned head, count;
};

struct vsend {
	struct vsend_config cfg;
	struct vsend_callbacks cb;
	struct vsend_stats st;
	struct vs_frame ring[VS_RING];
	struct vs_queue normal, repair;
	uint64_t seq;
	uint64_t next_send_us, last_video_us;
	uint32_t newest_id;
	bool have_frame;

	/* frames before (and including) `base` have been resolved by the receiver */
	uint32_t base;
	bool base_valid;

	/* rate adaptation */
	uint32_t target_kbps;
	unsigned fec_percent;
	double loss_ewma; /* permille, fast: drives the FEC overhead */
	double loss_slow; /* permille, about 200 ms of memory: a 10 ms interval holds ~14 packets, so one loss reads as 71 permille */
	uint64_t last_adjust_us;
	unsigned congested; /* consecutive reports with a standing queue */
	uint64_t last_idr_request_us, last_idr_submit_us;
	uint64_t last_lost_us, last_decrease_us; /* congestion memory: frames reported lost, last rate cut */
	uint64_t rtt_us; /* smoothed: first packet sent to the frame being reported held */
	uint64_t last_progress_us; /* when the receiver last reported seeing a newer frame than before */
	uint32_t fb_newest;
	bool fb_newest_set;
};

static bool q_push(struct vs_queue *q, struct vs_job j)
{
	if (q->count == VS_QUEUE) return false;
	q->jobs[(q->head + q->count++) % VS_QUEUE] = j;
	return true;
}

static bool q_pop(struct vs_queue *q, struct vs_job *j)
{
	if (!q->count) return false;
	*j = q->jobs[q->head];
	q->head = (q->head + 1) % VS_QUEUE;
	q->count--;
	return true;
}

struct vsend *vsend_create(const struct vsend_config *cfg, const struct vsend_callbacks *cb)
{
	struct vsend *vs = calloc(1, sizeof *vs);
	if (!vs) return NULL;
	vs->cfg = *cfg;
	if (cb) vs->cb = *cb;
	if (!vs->cfg.shard_size) vs->cfg.shard_size = 1200;
	if (!vs->cfg.fec_percent_min) vs->cfg.fec_percent_min = 5;
	if (!vs->cfg.fec_percent_max) vs->cfg.fec_percent_max = 30;
	if (!vs->cfg.pace_factor_percent) vs->cfg.pace_factor_percent = 150;
	if (!vs->cfg.keepalive_us) vs->cfg.keepalive_us = 100000;
	if (!vs->cfg.repair_gap_us) vs->cfg.repair_gap_us = 8000;
	if (!vs->cfg.pace_burst_us) vs->cfg.pace_burst_us = 2000;
	if (!vs->cfg.max_resend_bytes) vs->cfg.max_resend_bytes = 64 * 1024;
	if (!vs->cfg.min_bitrate_kbps) vs->cfg.min_bitrate_kbps = 1000;
	if (!vs->cfg.max_bitrate_kbps) vs->cfg.max_bitrate_kbps = 100000;
	vs->target_kbps = cfg->bitrate_kbps ? cfg->bitrate_kbps : 20000;
	vs->fec_percent = vs->cfg.fec_percent_min * 2;
	if (vs->fec_percent > vs->cfg.fec_percent_max) vs->fec_percent = vs->cfg.fec_percent_max;
	vs->normal.jobs = calloc(VS_QUEUE, sizeof(struct vs_job));
	vs->repair.jobs = calloc(VS_QUEUE, sizeof(struct vs_job));
	if (!vs->normal.jobs || !vs->repair.jobs) {
		vsend_destroy(vs);
		return NULL;
	}
	return vs;
}

void vsend_destroy(struct vsend *vs)
{
	if (!vs) return;
	for (int i = 0; i < VS_RING; i++) free(vs->ring[i].data);
	free(vs->normal.jobs);
	free(vs->repair.jobs);
	free(vs);
}

static struct vs_frame *frame_for(struct vsend *vs, uint32_t id)
{
	struct vs_frame *f = &vs->ring[id % VS_RING];
	return f->used && f->id == id ? f : NULL;
}

static uint64_t pace_bps(const struct vsend *vs)
{
	return (uint64_t)vs->target_kbps * 1000 * vs->cfg.pace_factor_percent / 100;
}

int vsend_submit(struct vsend *vs, uint32_t frame_id, uint32_t ref_frame_id, uint64_t capture_us, uint8_t vflags,
		 const uint8_t *data, uint32_t size, uint64_t now_us)
{
	struct vs_frame *f = &vs->ring[frame_id % VS_RING];
	uint8_t *copy = malloc(size ? size : 1);
	if (!copy) return -ENOMEM;
	memcpy(copy, data, size);

	struct vtx_frame vf;
	int rc = vtx_frame_init(&vf, frame_id, ref_frame_id, capture_us, vs->cfg.epoch, vflags, copy, size,
				vs->cfg.shard_size,
				/* every later frame depends on a keyframe: it gets the most protection, whatever the recent loss was (the very first
				 * one is sent before any loss has been measured, on a link that may have been idle) */
				(vflags & PROTO_VFLAG_IDR) ? vs->cfg.fec_percent_max : vs->fec_percent);
	if (rc != 0) { free(copy); return rc; }
	unsigned jobs = vtx_frame_packets(&vf);
	if (vs->normal.count + jobs > VS_QUEUE) { free(copy); return -EAGAIN; }

	free(f->data);
	memset(f, 0, sizeof *f);
	f->used = true;
	f->id = frame_id;
	f->ref = ref_frame_id;
	f->vf = vf;
	f->data = copy;
	f->vf.data = copy;
	unsigned total_data = 0;
	for (unsigned b = 0; b < vf.block_count; b++) {
		f->next_repair[b] = (uint8_t)(vf.blocks[b].data_shards + vf.blocks[b].parity_shards);
		total_data += vf.blocks[b].data_shards;
	}
	f->repair_budget = 2 * total_data + 16;

	for (unsigned b = 0; b < vf.block_count; b++)
		for (unsigned s = 0; s < vf.blocks[b].data_shards + vf.blocks[b].parity_shards; s++)
			q_push(&vs->normal, (struct vs_job){ frame_id, (uint8_t)b, (uint8_t)s, 0 });
	if (!vs->have_frame || (int32_t)(frame_id - vs->newest_id) > 0) vs->newest_id = frame_id;
	if (!vs->base_valid) { vs->base = frame_id - 1; vs->base_valid = true; }
	vs->have_frame = true;
	if (vflags & PROTO_VFLAG_IDR) vs->last_idr_submit_us = now_us ? now_us : 1;
	vs->st.frames++;
	return 0;
}

int vsend_next_packet(struct vsend *vs, uint64_t now_us, uint8_t *out, size_t cap)
{
	if (now_us < vs->next_send_us) return 0;
	struct vs_job j;
	for (;;) {
		bool got = q_pop(&vs->repair, &j) || q_pop(&vs->normal, &j);
		if (!got) break;
		struct vs_frame *f = frame_for(vs, j.frame_id);
		if (!f) continue; /* the frame was overwritten in the ring before this job came up */
		if ((j.flags & PROTO_PKT_FLAG_REPAIR) && f->held) continue; /* no longer needed */
		int n = vtx_packet_at(&f->vf, vs->cfg.session_id, vs->seq, (uint32_t)now_us, j.block, j.shard, j.flags, out, cap);
		if (n < 0) continue;
		vs->seq++;
		if (!f->first_send_us) f->first_send_us = now_us ? now_us : 1;
		vs->last_video_us = now_us;
		vs->st.packets++;
		vs->st.bytes += (uint64_t)n;
		if (j.flags & PROTO_PKT_FLAG_REPAIR) vs->st.repair_packets++;
		/* A late wake-up may catch up by up to pace_burst_us of credit, so timer granularity does not cap the rate: waking
		 * every millisecond must not mean one packet per millisecond. */
		uint64_t floor_us = now_us > vs->cfg.pace_burst_us ? now_us - vs->cfg.pace_burst_us : 0;
		uint64_t start = vs->next_send_us > floor_us ? vs->next_send_us : floor_us;
		vs->next_send_us = start + (uint64_t)n * 8 * 1000000 / pace_bps(vs);
		return n;
	}

	/* nothing to send: keep the link alive and let the phone learn the newest frame id */
	if (vs->have_frame && now_us >= vs->last_video_us + vs->cfg.keepalive_us && now_us >= vs->next_send_us &&
	    cap >= PROTO_PKT_HDR_SIZE + 16) {
		struct proto_pkt_hdr h = { .session_id = vs->cfg.session_id, .type = PROTO_PKT_KEEPALIVE, .seq = vs->seq++ };
		struct proto_keepalive ka = { .newest_frame_id = vs->newest_id, .now_us = now_us, .input_acked = 0 };
		int a = proto_pkt_hdr_write(&h, out, cap);
		int b = proto_keepalive_encode(&ka, out + a, cap - (size_t)a);
		if (a < 0 || b < 0) return 0;
		vs->last_video_us = now_us;
		vs->st.keepalives++;
		return a + b;
	}
	return 0;
}

uint64_t vsend_next_due_us(const struct vsend *vs, uint64_t now_us)
{
	if (!vs->normal.count && !vs->repair.count) {
		if (!vs->have_frame) return UINT64_MAX;
		uint64_t k = vs->last_video_us + vs->cfg.keepalive_us;
		return k > now_us ? k : now_us;
	}
	return vs->next_send_us > now_us ? vs->next_send_us : now_us;
}

size_t vsend_queued_bytes(const struct vsend *vs)
{
	return (size_t)(vs->normal.count + vs->repair.count) * (PROTO_PKT_HDR_SIZE + PROTO_VIDEO_HDR_SIZE + vs->cfg.shard_size);
}

uint64_t vsend_queue_delay_us(const struct vsend *vs)
{
	return (uint64_t)vsend_queued_bytes(vs) * 8 * 1000000 / pace_bps(vs);
}

unsigned vsend_unresolved(const struct vsend *vs)
{
	if (!vs->have_frame || !vs->base_valid) return 0;
	int32_t n = (int32_t)(vs->newest_id - vs->base);
	return n > 0 ? (unsigned)n : 0;
}

uint64_t vsend_rtt_us(const struct vsend *vs) { return vs->rtt_us; }

bool vsend_must_idr(const struct vsend *vs, uint64_t now_us)
{
	/* a healthy link leaves about rtt / frame interval frames in flight: allow twice that, and never fewer than 8 */
	unsigned limit = 8;
	if (vs->rtt_us) {
		unsigned rtt_frames = (unsigned)(2 * vs->rtt_us / 16667) + 4;
		if (rtt_frames > limit) limit = rtt_frames;
	}
	uint64_t last_idr = vs->last_idr_request_us > vs->last_idr_submit_us ? vs->last_idr_request_us : vs->last_idr_submit_us;
	if (last_idr && now_us < last_idr + 150000) return false;
	/* a keyframe cannot help while nothing reaches the receiver (a blackout): its newest frame id stops advancing. Wait
	 * until frames get through again. */
	if (!vs->last_progress_us || now_us > vs->last_progress_us + 100000) return false;
	/* Frames that are only waiting in our own send queue are not lost, and a keyframe queued behind them cannot arrive any sooner: it only makes
	 * the queue longer, which leaves more frames unresolved, which asks for another keyframe. A very large frame (the first keyframe of a page of
	 * fine text at a low starting rate) started exactly that spiral: idr 1 to 8 within seconds and a queue of 850 ms. */
	if (vsend_queue_delay_us(vs) > 150000) return false;
	return vsend_unresolved(vs) > limit;
}

uint32_t vsend_bitrate_kbps(const struct vsend *vs) { return vs->target_kbps; }
unsigned vsend_fec_percent(const struct vsend *vs) { return vs->fec_percent; }
const struct vsend_stats *vsend_stats(const struct vsend *vs) { return &vs->st; }

/* ---- feedback */

static void enqueue_repair(struct vsend *vs, struct vs_frame *f, unsigned block, unsigned count, bool all_data, uint64_t now_us)
{
	if (block >= f->vf.block_count) return;
	if (f->last_repair_us[block] && now_us < f->last_repair_us[block] + vs->cfg.repair_gap_us) return;
	const unsigned k = f->vf.blocks[block].data_shards;
	f->last_repair_us[block] = now_us ? now_us : 1;
	if (all_data) count = k;
	if (count > f->repair_budget) count = f->repair_budget;
	for (unsigned i = 0; i < count; i++) {
		unsigned shard;
		if (all_data || f->next_repair[block] > FEC_MAX_INDEX) {
			shard = i % k; /* out of fresh parity indices: resend original data shards */
		} else {
			shard = f->next_repair[block]++;
		}
		if (!q_push(&vs->repair, (struct vs_job){ f->id, (uint8_t)block, (uint8_t)shard, PROTO_PKT_FLAG_REPAIR })) return;
		f->repair_budget--;
	}
}

static void adapt(struct vsend *vs, const struct proto_feedback *fb, uint64_t now_us)
{
	vs->loss_ewma = vs->loss_ewma * 0.7 + fb->loss_permille * 0.3;
	vs->loss_slow = vs->loss_slow * 0.95 + fb->loss_permille * 0.05;
	/* FEC overhead follows the loss: about 3x the loss rate plus a margin, within the configured bounds */
	unsigned fec = vs->cfg.fec_percent_min + (unsigned)(vs->loss_ewma * 3 / 10.0 + 0.5);
	if (fec < vs->cfg.fec_percent_min) fec = vs->cfg.fec_percent_min;
	if (fec > vs->cfg.fec_percent_max) fec = vs->cfg.fec_percent_max;
	vs->fec_percent = fec;

	vs->congested = fb->queue_delay_us > 25000 ? vs->congested + 1 : 0;
	if (now_us < vs->last_adjust_us + 100000) return; /* adjust at most every 100 ms */
	uint32_t t = vs->target_kbps;
	bool recent_loss = vs->last_lost_us && now_us < vs->last_lost_us + 500000;
	if (vs->congested >= 3 || vs->loss_slow > 200) {
		/* A standing queue (25 ms or more for three reports) or severe loss (above 20%): cut in proportion to how deep the queue is, and
		 * when it is severe go below what actually got through. */
		unsigned q_ms = fb->queue_delay_us / 1000;
		unsigned keep = q_ms >= 100 ? 60 : q_ms >= 25 ? 100 - (q_ms - 25) * 2 / 3 : 92; /* percent of the rate kept */
		if (keep > 92) keep = 92;
		t = (uint32_t)((uint64_t)t * keep / 100);
		if (q_ms > 50 && fb->recv_kbps) {
			uint32_t cap = (uint32_t)(fb->recv_kbps * 0.85);
			if (cap < t) t = cap;
		}
		vs->last_adjust_us = vs->last_decrease_us = now_us;
	} else if (vs->congested == 0 && fb->queue_delay_us < 15000 && vs->loss_slow < 100 && !recent_loss &&
		   (!vs->last_decrease_us || now_us > vs->last_decrease_us + 1000000)) {
		t += t / 50 + 50; /* slow increase: about 2% per 100 ms, and only after a quiet second */
		vs->last_adjust_us = now_us;
	}
	if (t < vs->cfg.min_bitrate_kbps) t = vs->cfg.min_bitrate_kbps;
	if (t > vs->cfg.max_bitrate_kbps) t = vs->cfg.max_bitrate_kbps;
	vs->target_kbps = t;
}

int vsend_make_packet(struct vsend *vs, uint8_t type, const uint8_t *payload, size_t len, uint8_t *out, size_t cap)
{
	struct proto_pkt_hdr h = { .session_id = vs->cfg.session_id, .type = type, .seq = vs->seq };
	int a = proto_pkt_hdr_write(&h, out, cap);
	if (a < 0) return a;
	if (len > cap - (size_t)a) return -EMSGSIZE;
	if (len) memcpy(out + a, payload, len);
	vs->seq++;
	return a + (int)len;
}

int vsend_on_packet(struct vsend *vs, const uint8_t *pkt, size_t len, uint64_t now_us)
{
	struct proto_pkt_hdr h;
	int rc = proto_pkt_hdr_read(pkt, len, &h);
	if (rc != 0) return rc;
	if (h.session_id != vs->cfg.session_id || h.type != PROTO_PKT_FEEDBACK) return 0;
	struct proto_feedback fb;
	rc = proto_feedback_decode(pkt + PROTO_PKT_HDR_SIZE, len - PROTO_PKT_HDR_SIZE, &fb);
	if (rc != 0) return rc;
	vs->st.feedback++;
	if ((uint32_t)fb.queue_delay_us > vs->st.fb_queue_max_us) vs->st.fb_queue_max_us = (uint32_t)fb.queue_delay_us;
	if (fb.decode_ms_x10 > vs->st.fb_decode_max_x10) vs->st.fb_decode_max_x10 = fb.decode_ms_x10;
	if (fb.recv_kbps && (!vs->st.fb_recv_kbps_min || fb.recv_kbps < vs->st.fb_recv_kbps_min)) vs->st.fb_recv_kbps_min = fb.recv_kbps;
	if (!vs->fb_newest_set || (int32_t)(fb.newest_frame_id - vs->fb_newest) > 0) {
		vs->fb_newest = fb.newest_frame_id;
		vs->fb_newest_set = true;
		vs->last_progress_us = now_us ? now_us : 1;
	}

	/* frames the receiver has resolved since the last report: held, or given up */
	if (vs->base_valid && (int32_t)(fb.held_base - vs->base) > 0) {
		uint32_t lost_ids[64];
		unsigned n_lost = 0;
		for (uint32_t id = vs->base + 1; (int32_t)(fb.held_base - id) >= 0; id++) {
			uint32_t back = fb.held_base - id;
			bool held = back < 64 && ((fb.held_bitmap >> back) & 1);
			struct vs_frame *f = frame_for(vs, id);
			if (held) {
				vs->st.frames_held++;
				if (f) {
					f->held = true;
					if (f->first_send_us && now_us > f->first_send_us) {
						uint64_t s = now_us - f->first_send_us;
						vs->rtt_us = vs->rtt_us ? (vs->rtt_us * 7 + s) / 8 : s;
					}
				}
			} else {
				vs->st.frames_lost++;
				vs->last_lost_us = now_us ? now_us : 1;
				if (f) f->lost = true;
				if (n_lost < 64) lost_ids[n_lost++] = id;
			}
		}
		vs->base = fb.held_base;
		if (vs->cb.invalidate)
			for (unsigned i = 0; i < n_lost; i++) vs->cb.invalidate(lost_ids[i], vs->cb.user);

		/* The receiver gives up on every descendant of a lost frame, so those frames are as good as lost already. Without
		 * waiting a round trip for each report, drop them as references now: otherwise the encoder keeps predicting from
		 * the newest frame, a not-yet-reported descendant, and the chain of doomed frames never ends. */
		if (n_lost && vs->cb.invalidate) {
			for (uint32_t id = lost_ids[0] + 1; (int32_t)(vs->newest_id - id) >= 0; id++) {
				struct vs_frame *f = frame_for(vs, id);
				if (!f || f->held || f->lost || f->doomed || f->ref == PROTO_NO_REF) continue;
				struct vs_frame *r = frame_for(vs, f->ref);
				if (r && (r->lost || r->doomed)) {
					f->doomed = true;
					vs->cb.invalidate(id, vs->cb.user);
				}
			}
		}
	}
	/* The receiver repeats need_idr until an IDR has been decoded, which takes a round trip or more: act on it once per
	 * 150 ms, otherwise every report would trigger another keyframe. */
	uint64_t last_idr = vs->last_idr_request_us > vs->last_idr_submit_us ? vs->last_idr_request_us : vs->last_idr_submit_us;
	if ((fb.flags & PROTO_FB_NEED_IDR) && vs->cb.need_idr && (!last_idr || now_us >= last_idr + 150000)) {
		vs->last_idr_request_us = now_us ? now_us : 1;
		vs->cb.need_idr(vs->cb.user);
	}

	for (int i = 0; i < fb.nack_count; i++) {
		const struct proto_nack *n = &fb.nacks[i];
		vs->st.nacks++;
		struct vs_frame *f = frame_for(vs, n->frame_id);
		if (!f || f->held || f->lost) continue;
		if (n->block_index == 255 && n->need == VIDEO_NEED_ALL) {
			/* nothing of the frame arrived: resend it if it is small, otherwise leave it to invalidation */
			if (f->vf.size > vs->cfg.max_resend_bytes) continue;
			for (unsigned b = 0; b < f->vf.block_count; b++) enqueue_repair(vs, f, b, 0, true, now_us);
		} else if (n->need == VIDEO_NEED_ALL) {
			enqueue_repair(vs, f, n->block_index, 0, true, now_us);
		} else {
			enqueue_repair(vs, f, n->block_index, n->need, false, now_us);
		}
	}
	adapt(vs, &fb, now_us);
	return 0;
}

void vsend_reset_report(struct vsend *vs)
{
	vs->st.fb_queue_max_us = vs->st.fb_decode_max_x10 = vs->st.fb_recv_kbps_min = 0;
}
