/* End-to-end tests of the video sender and receiver over a simulated link: loss (independent and bursty), jitter and
 * reordering, blackouts, a bandwidth cap, a lost final frame on a static screen, and a lossy feedback path.
 *
 * Everything is deterministic: a simulated clock, a seeded generator, no sockets. The model encoder follows the real one's
 * reference rules (predict from the newest valid frame, at most 6 back, IDR when none is left). The model decoder keeps the
 * picture of every frame it decoded and checks each new picture against ground truth, so decoding a frame with a missing
 * or wrong reference is detected as corruption rather than assumed away. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vrecv.h"
#include "vsend.h"
#include "video.h"

static int failures;
#define CHECK(cond)                                                                                  \
	do {                                                                                         \
		if (!(cond)) {                                                                       \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);     \
			failures++;                                                                  \
		}                                                                                    \
	} while (0)

/* ---- random numbers */

struct rng { uint64_t s; };
static uint32_t rn(struct rng *r)
{
	r->s ^= r->s << 13;
	r->s ^= r->s >> 7;
	r->s ^= r->s << 17;
	return (uint32_t)(r->s >> 16);
}
static double rf(struct rng *r) { return (double)(rn(r) % 1000000) / 1e6; }

/* ---- the link */

#define MAXPKT 1600
#define LINK_CAP 4096

struct simpkt {
	bool used;
	uint64_t deliver_us;
	size_t len;
	uint8_t data[MAXPKT];
};

struct window { uint64_t start, end; };

struct link {
	struct simpkt q[LINK_CAP];
	uint64_t delay_us, jitter_us;
	double p_gb, p_bg, loss_g, loss_b; /* Gilbert-Elliott; equal loss_g and loss_b with p_gb = 0 is independent loss */
	bool bad;
	uint64_t rate_bps, queue_limit_bytes, busy_until;
	struct window outage[2];
	struct rng rng;
	uint64_t sent, dropped;
};

static void link_init(struct link *l, uint64_t seed)
{
	memset(l, 0, sizeof *l);
	l->rng.s = seed * 0x9e3779b97f4a7c15ull + 12345;
	l->delay_us = 5000;
}

static void link_send(struct link *l, uint64_t now, const uint8_t *data, size_t len)
{
	l->sent++;
	for (int i = 0; i < 2; i++)
		if (l->outage[i].end > l->outage[i].start && now >= l->outage[i].start && now < l->outage[i].end) { l->dropped++; return; }
	if (rf(&l->rng) < (l->bad ? l->p_bg : l->p_gb)) l->bad = !l->bad;
	if (rf(&l->rng) < (l->bad ? l->loss_b : l->loss_g)) { l->dropped++; return; }
	uint64_t t = now;
	if (l->rate_bps) {
		uint64_t start = l->busy_until > now ? l->busy_until : now;
		uint64_t backlog = l->busy_until > now ? (l->busy_until - now) * l->rate_bps / 8000000 : 0;
		if (l->queue_limit_bytes && backlog + len > l->queue_limit_bytes) { l->dropped++; return; } /* tail drop */
		l->busy_until = start + (uint64_t)len * 8000000 / l->rate_bps;
		t = l->busy_until;
	}
	t += l->delay_us + (l->jitter_us ? rn(&l->rng) % (l->jitter_us + 1) : 0);
	for (int i = 0; i < LINK_CAP; i++)
		if (!l->q[i].used) {
			l->q[i].used = true;
			l->q[i].deliver_us = t;
			l->q[i].len = len;
			memcpy(l->q[i].data, data, len);
			return;
		}
	l->dropped++; /* the model's own buffer is full */
}

/* the due packet with the earliest delivery time, if any */
static bool link_pop(struct link *l, uint64_t now, uint8_t *out, size_t *len)
{
	int best = -1;
	for (int i = 0; i < LINK_CAP; i++)
		if (l->q[i].used && l->q[i].deliver_us <= now && (best < 0 || l->q[i].deliver_us < l->q[best].deliver_us)) best = i;
	if (best < 0) return false;
	memcpy(out, l->q[best].data, l->q[best].len);
	*len = l->q[best].len;
	l->q[best].used = false;
	return true;
}

/* ---- model encoder and decoder */

#define MAXFRAMES 6000
#define FRAME_US 16667u
#define DPB_BACK 6

static uint64_t hash2(uint64_t a, uint32_t id)
{
	uint64_t x = a ^ ((uint64_t)id * 0x9e3779b97f4a7c15ull);
	x ^= x >> 29; x *= 0xbf58476d1ce4e5b9ull; x ^= x >> 32;
	return x | 1;
}

struct sim;
struct metrics {
	unsigned frames_made, frames_skipped, displayed, corrupt, idr_sent, idr_decoded;
	uint64_t max_gap_us;       /* longest time without a displayed frame, after warm-up */
	uint64_t max_gap_at_us;    /* when that freeze ended */
	uint64_t lat[MAXFRAMES];   /* capture-to-display latency of displayed frames */
	uint64_t lat_t[MAXFRAMES]; /* time of display */
	unsigned n_lat;
	uint64_t last_display_us;
	uint32_t final_kbps;
	uint64_t fwd_sent, fwd_dropped, bytes_sent, repair_packets;
	uint64_t first_display_after_us; /* first display at or after `probe_us` */
	uint64_t probe_us;
	unsigned nacks, invalidations;
};

struct sim {
	uint64_t now;
	struct link fwd, rev;
	struct vsend *vs;
	struct vrecv *vr;
	struct rng rng;
	struct metrics m;

	/* model encoder */
	uint32_t next_id;
	bool need_idr;
	bool valid[MAXFRAMES];
	uint64_t truth[MAXFRAMES];
	uint32_t ref_of[MAXFRAMES];

	/* model decoder */
	bool dpb_has[MAXFRAMES];
	uint64_t dpb_hash[MAXFRAMES];
	uint64_t warmup_us;
	bool decoder_broken; /* test hook: make the decoder fail once */
};

static void cb_invalidate(uint32_t lost, void *u)
{
	struct sim *s = u;
	s->m.invalidations++;
	if (lost < MAXFRAMES) s->valid[lost] = false;
}
static void cb_need_idr(void *u) { ((struct sim *)u)->need_idr = true; }

#define PAYLOAD_HDR 16u /* id, ref, truth */

static int cb_decode(const struct vrecv_frame *f, void *u)
{
	struct sim *s = u;
	if (s->decoder_broken) { s->decoder_broken = false; return -1; }
	if (f->size < PAYLOAD_HDR || f->frame_id >= MAXFRAMES) { s->m.corrupt++; return 0; }
	uint32_t id, ref;
	uint64_t truth;
	memcpy(&id, f->data, 4); memcpy(&ref, f->data + 4, 4); memcpy(&truth, f->data + 8, 8);
	bool ok = id == f->frame_id && ref == f->ref_frame_id;
	/* the rest of the payload must be exactly what the encoder wrote (reassembly correctness) */
	for (size_t i = PAYLOAD_HDR; ok && i < f->size; i++)
		if (f->data[i] != (uint8_t)(id * 31u + i)) ok = false;
	/* the picture: built from the reference picture, which the decoder must actually have */
	uint64_t pic;
	if (ref == PROTO_NO_REF) {
		pic = hash2(0, id);
	} else if (ref < MAXFRAMES && s->dpb_has[ref]) {
		pic = hash2(s->dpb_hash[ref], id);
	} else {
		pic = 0xdeadbeefdeadbeefull; /* garbage: the reference is missing */
	}
	if (!ok || pic != truth) s->m.corrupt++;
	s->dpb_has[id] = true;
	s->dpb_hash[id] = pic;
	if (ref == PROTO_NO_REF) {
		s->m.idr_decoded++;
		for (uint32_t i = 0; i < id; i++) s->dpb_has[i] = false; /* an IDR flushes the decoder */
	}

	struct metrics *m = &s->m;
	if (s->now >= s->warmup_us && m->n_lat < MAXFRAMES) {
		if (m->last_display_us >= s->warmup_us && s->now - m->last_display_us > m->max_gap_us) { m->max_gap_us = s->now - m->last_display_us; m->max_gap_at_us = s->now; }
		m->lat[m->n_lat] = s->now - f->capture_us;
		m->lat_t[m->n_lat++] = s->now;
		m->displayed++;
	}
	if (m->probe_us && !m->first_display_after_us && s->now >= m->probe_us) m->first_display_after_us = s->now;
	m->last_display_us = s->now;
	return 0;
}

/* The model encoder: IDR when asked or when nothing valid is left, otherwise predict from the newest valid frame within
 * DPB_BACK. Returns the frame, or -1 if the sender's queue is too long (the capture is skipped before encoding). */
static void encode_frame(struct sim *s, uint32_t size_hint)
{
	if (vsend_queue_delay_us(s->vs) > 20000) { s->m.frames_skipped++; return; }
	uint32_t id = s->next_id;
	if (id >= MAXFRAMES) return;
	if (vsend_must_idr(s->vs, s->now)) s->need_idr = true; /* the reference chain may be doomed */
	uint32_t ref = PROTO_NO_REF;
	if (!s->need_idr)
		for (uint32_t back = 1; back <= DPB_BACK && back <= id; back++)
			if (s->valid[id - back]) { ref = id - back; break; }
	bool idr = ref == PROTO_NO_REF;
	if (idr) {
		for (uint32_t i = 0; i < id; i++) s->valid[i] = false;
		s->need_idr = false;
		s->m.idr_sent++;
	}
	uint32_t size = idr ? size_hint * 3 : size_hint;
	size = size * (80 + rn(&s->rng) % 41) / 100;
	if (size < 200) size = 200;
	uint8_t *buf = malloc(size);
	uint64_t truth = hash2(ref == PROTO_NO_REF ? 0 : s->truth[ref], id);
	memcpy(buf, &id, 4); memcpy(buf + 4, &ref, 4); memcpy(buf + 8, &truth, 8);
	for (uint32_t i = PAYLOAD_HDR; i < size; i++) buf[i] = (uint8_t)(id * 31u + i);
	int rc = vsend_submit(s->vs, id, ref, s->now, idr ? PROTO_VFLAG_IDR : 0, buf, size, s->now);
	free(buf);
	if (rc != 0) { s->m.frames_skipped++; return; }
	s->truth[id] = truth;
	s->ref_of[id] = ref;
	s->valid[id] = true;
	s->next_id++;
	s->m.frames_made++;
}

/* ---- run a scenario */

struct scenario {
	const char *name;
	uint64_t seed;
	uint64_t duration_us, warmup_us;
	uint64_t fwd_delay_us, fwd_jitter_us, rev_delay_us;
	double loss_g, loss_b, p_gb, p_bg;      /* forward link */
	double rev_loss;                         /* feedback path */
	uint64_t rate_bps, queue_limit_bytes;    /* forward bandwidth cap (0 = none) */
	struct window outage[2];                 /* forward blackouts */
	uint32_t bitrate_kbps;                   /* initial and maximum */
	uint64_t source_stop_us;                 /* stop producing frames (static screen), 0 = never */
	uint64_t probe_us;                       /* record the first display at or after this time */
	uint64_t decoder_break_us;               /* make the decoder fail once at this time */
};

static struct sim *run(const struct scenario *sc)
{
	struct sim *s = calloc(1, sizeof *s);
	s->rng.s = sc->seed * 7919 + 1;
	link_init(&s->fwd, sc->seed);
	link_init(&s->rev, sc->seed + 1000);
	s->fwd.delay_us = sc->fwd_delay_us; s->fwd.jitter_us = sc->fwd_jitter_us;
	s->fwd.loss_g = sc->loss_g; s->fwd.loss_b = sc->loss_b; s->fwd.p_gb = sc->p_gb; s->fwd.p_bg = sc->p_bg;
	s->fwd.rate_bps = sc->rate_bps; s->fwd.queue_limit_bytes = sc->queue_limit_bytes;
	s->fwd.outage[0] = sc->outage[0]; s->fwd.outage[1] = sc->outage[1];
	s->rev.delay_us = sc->rev_delay_us; s->rev.loss_g = s->rev.loss_b = sc->rev_loss;
	s->warmup_us = sc->warmup_us;
	s->m.probe_us = sc->probe_us;

	struct vsend_config vc = { .session_id = 0x1234abcd, .bitrate_kbps = sc->bitrate_kbps, .max_bitrate_kbps = sc->bitrate_kbps,
				   .min_bitrate_kbps = 1500 };
	struct vsend_callbacks vcb = { .invalidate = cb_invalidate, .need_idr = cb_need_idr, .user = s };
	s->vs = vsend_create(&vc, &vcb);
	struct vrecv_config rc = { .session_id = 0x1234abcd };
	struct vrecv_callbacks rcb = { .decode = cb_decode, .user = s };
	s->vr = vrecv_create(&rc, &rcb);

	uint8_t buf[MAXPKT];
	uint64_t next_frame = 1000, next_fb = 10000;
	bool broke = false;
	for (s->now = 0; s->now < sc->duration_us; s->now += 500) {
		while (s->now >= next_frame) {
			if (!sc->source_stop_us || next_frame < sc->source_stop_us)
				encode_frame(s, vsend_bitrate_kbps(s->vs) * 1000u / 8 / 60);
			next_frame += FRAME_US;
		}
		if (sc->decoder_break_us && !broke && s->now >= sc->decoder_break_us) { s->decoder_broken = true; broke = true; }

		int n;
		/* A real event loop wakes at timer granularity, not at each packet's due time: the sender is serviced once per
		 * 500 us step, and the pacer's burst allowance must let it keep up with its rate (it once sent one packet per
		 * wake-up, about 10 Mbit/s whatever the target was). */
		while ((n = vsend_next_packet(s->vs, s->now, buf, sizeof buf)) > 0) link_send(&s->fwd, s->now, buf, (size_t)n);
		size_t len;
		while (link_pop(&s->fwd, s->now, buf, &len)) vrecv_on_packet(s->vr, buf, len, s->now);
		vrecv_tick(s->vr, s->now);
		if (s->now >= next_fb) {
			next_fb += 10000;
			n = vrecv_feedback_packet(s->vr, s->now, buf, sizeof buf);
			if (n > 0) link_send(&s->rev, s->now, buf, (size_t)n);
		}
		while (link_pop(&s->rev, s->now, buf, &len)) vsend_on_packet(s->vs, buf, len, s->now);
	}
	s->m.final_kbps = vsend_bitrate_kbps(s->vs);
	s->m.fwd_sent = s->fwd.sent;
	s->m.fwd_dropped = s->fwd.dropped;
	s->m.bytes_sent = vsend_stats(s->vs)->bytes;
	s->m.repair_packets = vsend_stats(s->vs)->repair_packets;
	s->m.nacks = (unsigned)vsend_stats(s->vs)->nacks;
	return s;
}

static void destroy(struct sim *s)
{
	vsend_destroy(s->vs);
	vrecv_destroy(s->vr);
	free(s);
}

static int cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
	return x < y ? -1 : x > y;
}

/* percentile of latencies displayed at or after `from_us` */
static double lat_pct(const struct sim *s, uint64_t from_us, double p)
{
	static uint64_t v[MAXFRAMES];
	unsigned n = 0;
	for (unsigned i = 0; i < s->m.n_lat; i++)
		if (s->m.lat_t[i] >= from_us) v[n++] = s->m.lat[i];
	if (!n) return -1;
	qsort(v, n, sizeof *v, cmp_u64);
	return v[(unsigned)((n - 1) * p)] / 1000.0;
}

static void report(const char *name, const struct sim *s, double expected_frames)
{
	printf("  %-34s displayed %5.1f%% (%u/%u made, %u skipped) corrupt %u  idr %u  freeze max %6.1f ms  "
	       "latency p50/p99 %5.1f/%6.1f ms  loss %4.1f%%  repairs %llu  kbps %u\n",
	       name, 100.0 * s->m.displayed / (expected_frames > 0 ? expected_frames : 1), s->m.displayed, s->m.frames_made,
	       s->m.frames_skipped, s->m.corrupt, s->m.idr_sent, s->m.max_gap_us / 1000.0, lat_pct(s, s->warmup_us, 0.5),
	       lat_pct(s, s->warmup_us, 0.99), 100.0 * s->m.fwd_dropped / (s->m.fwd_sent ? s->m.fwd_sent : 1),
	       (unsigned long long)s->m.repair_packets, s->m.final_kbps);
}

/* ---- scenarios */

static struct scenario base(const char *name, uint64_t seed)
{
	struct scenario sc;
	memset(&sc, 0, sizeof sc);
	sc.name = name;
	sc.seed = seed;
	sc.duration_us = 8000000;
	sc.warmup_us = 1000000;
	sc.fwd_delay_us = 5000;
	sc.rev_delay_us = 5000;
	sc.bitrate_kbps = 20000;
	return sc;
}

static double frames_in(const struct scenario *sc) { return (double)(sc->duration_us - sc->warmup_us) / FRAME_US; }

static void test_clean(void)
{
	struct scenario sc = base("clean link", 1);
	struct sim *s = run(&sc);
	report(sc.name, s, frames_in(&sc));
	CHECK(s->m.corrupt == 0);
	CHECK(s->m.displayed >= frames_in(&sc) * 0.995);
	CHECK(s->m.idr_sent == 1);
	CHECK(s->m.repair_packets == 0);              /* no false NACKs on a clean link */
	CHECK(s->m.final_kbps == 20000);              /* and the controller does not back off */
	CHECK(lat_pct(s, s->warmup_us, 0.99) < 30.0);
	CHECK(s->m.max_gap_us < 40000);
	destroy(s);
}

static void test_random_loss(void)
{
	for (uint64_t seed = 1; seed <= 5; seed++) {
		struct scenario sc = base("2% loss, 8 ms +/- 4 ms jitter", seed);
		sc.fwd_delay_us = 8000; sc.fwd_jitter_us = 8000;
		sc.loss_g = sc.loss_b = 0.02;
		struct sim *s = run(&sc);
		report(sc.name, s, frames_in(&sc));
		CHECK(s->m.corrupt == 0);
		CHECK(s->m.displayed >= frames_in(&sc) * 0.99);
		CHECK(s->m.max_gap_us < 60000);
		CHECK(s->m.idr_sent == 1);
		CHECK(lat_pct(s, s->warmup_us, 0.99) < 45.0);
		CHECK(s->m.final_kbps >= 5000);               /* mild loss must not collapse the bitrate */
		destroy(s);
	}
}

static void test_bursty_loss(void)
{
	for (uint64_t seed = 1; seed <= 5; seed++) {
		struct scenario sc = base("bursty loss (~3%, bursts of 5)", seed);
		sc.fwd_jitter_us = 4000;
		sc.p_gb = 0.008; sc.p_bg = 0.2; sc.loss_g = 0.0; sc.loss_b = 0.8;
		struct sim *s = run(&sc);
		report(sc.name, s, frames_in(&sc));
		CHECK(s->m.corrupt == 0);
		CHECK(s->m.displayed >= frames_in(&sc) * 0.98);
		CHECK(s->m.max_gap_us < 120000);
		CHECK(s->m.idr_sent == 1);
		CHECK(lat_pct(s, s->warmup_us, 0.99) < 90.0);
		CHECK(s->m.final_kbps >= 5000);
		destroy(s);
	}
}

static void test_blackouts(void)
{
	struct scenario sc = base("70 ms blackout", 3);
	sc.outage[0] = (struct window){ 4000000, 4070000 };
	sc.probe_us = 4070000;
	struct sim *s = run(&sc);
	report(sc.name, s, frames_in(&sc));
	printf("      first display %.1f ms after the blackout ended, %u invalidations, %u IDRs\n",
	       (s->m.first_display_after_us - sc.probe_us) / 1000.0, s->m.invalidations, s->m.idr_sent);
	CHECK(s->m.corrupt == 0);
	CHECK(s->m.first_display_after_us && s->m.first_display_after_us - sc.probe_us < 60000);
	CHECK(s->m.max_gap_us < 150000);
	CHECK(s->m.idr_sent == 1);                    /* the lost frames were repaired or invalidated, no keyframe needed */
	destroy(s);

	struct scenario sc2 = base("400 ms blackout", 3);
	sc2.outage[0] = (struct window){ 4000000, 4400000 };
	sc2.probe_us = 4400000;
	s = run(&sc2);
	report(sc2.name, s, frames_in(&sc2));
	printf("      first display %.1f ms after the blackout ended, %u invalidations, %u IDRs\n",
	       (s->m.first_display_after_us - sc2.probe_us) / 1000.0, s->m.invalidations, s->m.idr_sent);
	CHECK(s->m.corrupt == 0);
	CHECK(s->m.first_display_after_us && s->m.first_display_after_us - sc2.probe_us < 100000);
	CHECK(s->m.max_gap_us < 700000);
	CHECK(s->m.idr_sent >= 2 && s->m.idr_sent <= 3); /* too long for any retained reference, but no IDR storm into a dead link */
	destroy(s);
}

static void test_bandwidth_cap(void)
{
	struct scenario sc = base("10 Mbit/s cap, 30 Mbit/s start", 4);
	sc.duration_us = 14000000;
	sc.warmup_us = 8000000; /* measure after the controller has had time to settle */
	sc.bitrate_kbps = 30000;
	sc.rate_bps = 10000000;
	sc.queue_limit_bytes = 125000; /* about 100 ms of buffering at 10 Mbit/s */
	struct sim *s = run(&sc);
	report(sc.name, s, frames_in(&sc));
	CHECK(s->m.corrupt == 0);
	CHECK(s->m.final_kbps <= 11500);
	CHECK(lat_pct(s, s->warmup_us, 0.99) < 130.0);          /* latency does not keep growing */
	CHECK(s->m.displayed >= frames_in(&sc) * 0.95);
	destroy(s);
}

static void test_lost_final_frame(void)
{
	struct scenario sc = base("last frame lost, static screen", 5);
	sc.source_stop_us = 4000000;           /* nothing changes after this */
	sc.duration_us = 6000000;
	sc.warmup_us = 0;
	sc.outage[0] = (struct window){ 3990000, 4030000 }; /* swallows the final frame */
	struct sim *s = run(&sc);
	/* every frame that was made must have been displayed, including the last one */
	printf("  %-34s made %u displayed %u (last frame id %u)\n", sc.name, s->m.frames_made, s->m.displayed, s->next_id - 1);
	CHECK(s->m.corrupt == 0);
	CHECK(s->m.frames_made > 100);
	CHECK(s->dpb_has[s->next_id - 1]);
	destroy(s);
}

static void test_lossy_feedback(void)
{
	struct scenario sc = base("2% loss, 20% feedback loss", 6);
	sc.loss_g = sc.loss_b = 0.02;
	sc.rev_loss = 0.20;
	struct sim *s = run(&sc);
	report(sc.name, s, frames_in(&sc));
	CHECK(s->m.corrupt == 0);
	CHECK(s->m.displayed >= frames_in(&sc) * 0.99);
	CHECK(s->m.max_gap_us < 100000);
	destroy(s);
}

static void test_decoder_error(void)
{
	struct scenario sc = base("decoder error mid-stream", 7);
	sc.decoder_break_us = 4000000;
	struct sim *s = run(&sc);
	report(sc.name, s, frames_in(&sc));
	CHECK(s->m.corrupt == 0);
	CHECK(s->m.idr_sent == 2); /* the first, and one for the error: not a storm of them */
	CHECK(s->m.max_gap_us < 150000);
	destroy(s);
}

/* The PC numbers every datagram it sends in one sequence (video, keepalives, input acknowledgements). A receiver that is not shown the
 * acknowledgements sees gaps and reports loss, which once made the PC cut the bitrate whenever the phone was touched. */
static int ack_decode(const struct vrecv_frame *f, void *user) { (void)f; (void)user; return 0; }

static unsigned loss_with_acks(bool show_acks)
{
	struct vsend_config vc = { .session_id = 0x1234abcd, .bitrate_kbps = 20000, .max_bitrate_kbps = 20000, .min_bitrate_kbps = 1500 };
	struct vsend_callbacks vcb = { 0 };
	struct vsend *vs = vsend_create(&vc, &vcb);
	struct vrecv_config rc = { .session_id = 0x1234abcd };
	struct vrecv_callbacks rcb = { .decode = ack_decode, .user = NULL };
	struct vrecv *vr = vrecv_create(&rc, &rcb);
	uint8_t buf[MAXPKT];
	unsigned loss = 0;
	uint64_t now = 1000;
	uint8_t frame[600];
	memset(frame, 0x55, sizeof frame);
	CHECK(vsend_submit(vs, 1, PROTO_NO_REF, now, PROTO_VFLAG_IDR, frame, sizeof frame, now) == 0); /* the receiver starts on a keyframe */
	for (int guard = 0; guard < 64; guard++, now += 100) {
		int n = vsend_next_packet(vs, now + 1000000, buf, sizeof buf);
		if (n <= 0) break;
		vrecv_on_packet(vr, buf, (size_t)n, now);
	}
	(void)vrecv_feedback_packet(vr, now + 20000, buf, sizeof buf); /* starts the first interval */
	now += 20000;
	for (int round = 0; round < 40; round++) {
		for (int i = 0; i < 5; i++) { /* some packets of the PC's own: acknowledgements */
			uint8_t payload[4] = { (uint8_t)round, 0, 0, 0 };
			int n = vsend_make_packet(vs, PROTO_PKT_INPUT_ACK, payload, sizeof payload, buf, sizeof buf);
			if (n > 0 && show_acks) vrecv_on_packet(vr, buf, (size_t)n, now);
			now += 1000;
		}
		int n = vsend_make_packet(vs, PROTO_PKT_KEEPALIVE, (const uint8_t[]){ 0, 0, 0, 0, 0 }, 5, buf, sizeof buf);
		if (n > 0) vrecv_on_packet(vr, buf, (size_t)n, now);
		now += 1000;
		uint8_t fb[MAXPKT];
		int fn = vrecv_feedback_packet(vr, now + 20000, fb, sizeof fb);
		struct proto_feedback f;
		if (fn > PROTO_PKT_HDR_SIZE && proto_feedback_decode(fb + PROTO_PKT_HDR_SIZE, (size_t)fn - PROTO_PKT_HDR_SIZE, &f) == 0 && f.loss_permille > loss)
			loss = f.loss_permille;
		now += 20000;
	}
	vrecv_destroy(vr);
	vsend_destroy(vs);
	return loss;
}

static void test_ack_accounting(void)
{
	printf("  input acknowledgements are counted as received: ");
	unsigned shown = loss_with_acks(true), hidden = loss_with_acks(false);
	CHECK(shown == 0);
	CHECK(hidden > 200); /* what the app used to report: far above the 20%% that makes the PC cut the bitrate */
	printf("loss %u permille (acks shown), %u permille (hidden)\n", shown, hidden);
}

int main(void)
{
	puts("sender and receiver over a simulated link:");
	test_clean();
	test_random_loss();
	test_bursty_loss();
	test_blackouts();
	test_bandwidth_cap();
	test_lost_final_frame();
	test_lossy_feedback();
	test_decoder_error();
	test_ack_accounting();
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	puts("stream: all checks passed");
	return 0;
}
