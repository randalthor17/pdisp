/* Tests for video packetisation and reassembly (proto/video.c). */
#include "video.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond)                                                                                  \
	do {                                                                                         \
		if (!(cond)) {                                                                       \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);     \
			failures++;                                                                  \
		}                                                                                    \
	} while (0)

#define S 1200u
#define PKT (PROTO_PKT_HDR_SIZE + PROTO_VIDEO_HDR_SIZE + S)
#define SESSION 0xabcdef01u

static uint64_t rng = 0x853c49e6748fea9bull;
static uint32_t rnd(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 7;
	rng ^= rng << 17;
	return (uint32_t)(rng >> 16);
}

static uint8_t *random_bytes(size_t n)
{
	uint8_t *p = malloc(n ? n : 1);
	for (size_t i = 0; i < n; i++) p[i] = (uint8_t)rnd();
	return p;
}

struct pktlist {
	uint8_t *buf; /* n packets of PKT bytes */
	unsigned n, cap;
};

static void pl_add(struct pktlist *l, const struct vtx_frame *f, unsigned block, unsigned shard, uint8_t flags, uint64_t seq)
{
	if (l->n == l->cap) {
		l->cap = l->cap ? l->cap * 2 : 64;
		l->buf = realloc(l->buf, (size_t)l->cap * PKT);
	}
	int rc = vtx_packet(f, SESSION, seq, block, shard, flags, l->buf + (size_t)l->n * PKT, PKT);
	if (rc != (int)PKT) { fprintf(stderr, "vtx_packet returned %d\n", rc); failures++; }
	l->n++;
}

/* every initial packet of the frame, data shards first, then parity, block by block */
static void pl_initial(struct pktlist *l, const struct vtx_frame *f, uint64_t *seq)
{
	for (unsigned b = 0; b < f->block_count; b++)
		for (unsigned s = 0; s < f->blocks[b].data_shards + f->blocks[b].parity_shards; s++) pl_add(l, f, b, s, 0, (*seq)++);
}

/* shuffles the packets, and `mask` (if given) with them so it still refers to the same packets */
static void pl_shuffle(struct pktlist *l, bool *mask)
{
	uint8_t tmp[PKT];
	for (unsigned i = l->n; i > 1; i--) {
		unsigned j = rnd() % i;
		memcpy(tmp, l->buf + (size_t)(i - 1) * PKT, PKT);
		memcpy(l->buf + (size_t)(i - 1) * PKT, l->buf + (size_t)j * PKT, PKT);
		memcpy(l->buf + (size_t)j * PKT, tmp, PKT);
		if (mask) { bool t = mask[i - 1]; mask[i - 1] = mask[j]; mask[j] = t; }
	}
}

/* feeds packets; returns the number of completed frames, and the last one's data in *out */
static int feed_all(struct vrx *rx, const struct pktlist *l, const bool *drop, uint64_t now, struct vrx_frame *out)
{
	int done = 0;
	for (unsigned i = 0; i < l->n; i++) {
		if (drop && drop[i]) continue;
		struct vrx_frame f;
		int rc = vrx_push(rx, l->buf + (size_t)i * PKT, PKT, now, &f);
		if (rc == 1) { done++; if (out) *out = f; }
	}
	return done;
}

/* ---- tests */

static void test_geometry(void)
{
	struct vtx_frame f;
	CHECK(vtx_frame_init(&f, 1, PROTO_NO_REF, 0, 0, 0, (const uint8_t *)"x", 1, S, 10) == 0);
	CHECK(f.block_count == 1 && f.blocks[0].data_shards == 1 && f.blocks[0].parity_shards == 1);

	uint8_t *d = random_bytes(240000);
	CHECK(vtx_frame_init(&f, 1, 0, 0, 0, 0, d, 240000, S, 10) == 0);
	CHECK(f.block_count == 1 && f.blocks[0].data_shards == 200 && f.blocks[0].parity_shards == 20);
	free(d);
	d = random_bytes(240001);
	CHECK(vtx_frame_init(&f, 1, 0, 0, 0, 0, d, 240001, S, 10) == 0);
	CHECK(f.block_count == 2 && f.blocks[0].data_shards == 101 && f.blocks[1].data_shards == 100); /* balanced */
	CHECK(f.blocks[0].first_shard == 0 && f.blocks[1].first_shard == 101);
	CHECK(vtx_frame_packets(&f) == 101 + 11 + 100 + 10);
	free(d);

	/* bad arguments */
	uint8_t one = 1;
	CHECK(vtx_frame_init(&f, 1, 0, 0, 0, 0, &one, 0, S, 10) == -EINVAL);
	CHECK(vtx_frame_init(&f, 1, 0, 0, 0, 0, &one, 1, 32, 10) == -EINVAL);
	CHECK(vtx_frame_init(&f, 1, 0, 0, 0, 0, &one, 1, 2000, 10) == -EINVAL);
	CHECK(vtx_frame_init(&f, 1, 0, 0, 0, 0, &one, 1, S, 101) == -EINVAL);
	CHECK(vtx_frame_init(&f, 1, 0, 0, 0, 0, &one, 1, S, 0) == 0 && f.blocks[0].parity_shards == 0);
	uint8_t pk[PKT];
	CHECK(vtx_packet(&f, 1, 1, 1, 0, 0, pk, sizeof pk) == -EINVAL);            /* no such block */
	CHECK(vtx_packet(&f, 1, 1, 0, 255, 0, pk, sizeof pk) == -EINVAL);          /* past the last shard index */
	CHECK(vtx_packet(&f, 1, 1, 0, 0, 0, pk, PKT - 1) == -EMSGSIZE);
}

static void test_sizes_in_order(void)
{
	static const uint32_t sizes[] = { 1, 1199, 1200, 1201, 2400, 2401, 100000, 215999, 240000, 240001, 700000 };
	static const unsigned fecs[] = { 0, 10, 30 };
	uint64_t seq = 1;
	for (size_t i = 0; i < sizeof sizes / sizeof *sizes; i++)
		for (size_t j = 0; j < sizeof fecs / sizeof *fecs; j++) {
			uint8_t *data = random_bytes(sizes[i]);
			struct vtx_frame f;
			CHECK(vtx_frame_init(&f, 7, 6, 123456789, 0, PROTO_VFLAG_IDR, data, sizes[i], S, fecs[j]) == 0);
			struct pktlist l = { 0 };
			pl_initial(&l, &f, &seq);
			CHECK(l.n == vtx_frame_packets(&f));

			struct vrx *rx = vrx_create(S);
			struct vrx_frame out = { 0 };
			CHECK(feed_all(rx, &l, NULL, 1000, &out) == 1);
			CHECK(out.size == sizes[i] && memcmp(out.data, data, sizes[i]) == 0);
			CHECK(out.frame_id == 7 && out.ref_frame_id == 6 && out.capture_us == 123456789 && out.vflags == PROTO_VFLAG_IDR);
			CHECK(vrx_stats(rx)->frames_complete == 1);
			vrx_destroy(rx);
			free(l.buf);
			free(data);
		}
}

/* Lose up to the parity count in every block, in random order: the frame must still come out. */
static void test_loss_within_parity(void)
{
	static const uint32_t sizes[] = { 1, 5000, 50000, 100000, 300000, 700000 };
	uint64_t seq = 1;
	for (size_t i = 0; i < sizeof sizes / sizeof *sizes; i++)
		for (int trial = 0; trial < 20; trial++) {
			uint8_t *data = random_bytes(sizes[i]);
			struct vtx_frame f;
			CHECK(vtx_frame_init(&f, 3, 2, 99, 0, 0, data, sizes[i], S, 20) == 0);
			struct pktlist l = { 0 };
			pl_initial(&l, &f, &seq);

			bool *drop = calloc(l.n, 1);
			unsigned base = 0;
			for (unsigned b = 0; b < f.block_count; b++) { /* drop exactly the parity count within each block */
				unsigned n = f.blocks[b].data_shards + f.blocks[b].parity_shards;
				for (unsigned lost = 0; lost < f.blocks[b].parity_shards;) {
					unsigned x = base + rnd() % n;
					if (!drop[x]) { drop[x] = true; lost++; }
				}
				base += n;
			}
			pl_shuffle(&l, drop); /* arrival order must not matter */

			struct vrx *rx = vrx_create(S);
			struct vrx_frame out = { 0 };
			CHECK(feed_all(rx, &l, drop, 1000, &out) == 1);
			CHECK(out.size == sizes[i] && memcmp(out.data, data, sizes[i]) == 0);
			vrx_destroy(rx);
			free(drop);
			free(l.buf);
			free(data);
		}
}

static void test_duplicates_and_reorder(void)
{
	uint64_t seq = 1;
	uint8_t *data = random_bytes(60000);
	struct vtx_frame f;
	CHECK(vtx_frame_init(&f, 9, 8, 5, 0, 0, data, 60000, S, 10) == 0);
	struct pktlist l = { 0 };
	pl_initial(&l, &f, &seq);
	unsigned n0 = l.n;
	for (unsigned i = 0; i < n0; i++) { /* every packet twice */
		l.n++;
		if (l.n > l.cap) { l.cap = l.n * 2; l.buf = realloc(l.buf, (size_t)l.cap * PKT); }
		memcpy(l.buf + (size_t)(l.n - 1) * PKT, l.buf + (size_t)i * PKT, PKT);
	}
	pl_shuffle(&l, NULL);
	struct vrx *rx = vrx_create(S);
	struct vrx_frame out = { 0 };
	CHECK(feed_all(rx, &l, NULL, 1000, &out) == 1); /* completes exactly once */
	CHECK(memcmp(out.data, data, 60000) == 0);
	CHECK(vrx_stats(rx)->duplicates + vrx_stats(rx)->late >= n0 - f.blocks[0].data_shards);
	vrx_destroy(rx);
	free(l.buf);
	free(data);
}

static void test_nack_and_repair(void)
{
	uint64_t seq = 1;
	uint8_t *data = random_bytes(300000); /* 250 data shards: two blocks of 125 */
	struct vtx_frame f;
	CHECK(vtx_frame_init(&f, 20, 19, 7, 0, 0, data, 300000, S, 10) == 0);
	CHECK(f.block_count == 2 && f.blocks[0].data_shards == 125 && f.blocks[0].parity_shards == 13);

	struct pktlist l = { 0 };
	pl_initial(&l, &f, &seq);
	bool *drop = calloc(l.n, 1);
	/* block 0 loses 20 shards (parity 13: 7 short), block 1 loses 5 (within parity) */
	unsigned n0 = f.blocks[0].data_shards + f.blocks[0].parity_shards, n1 = f.blocks[1].data_shards + f.blocks[1].parity_shards;
	for (unsigned lost = 0; lost < 20;) { unsigned x = rnd() % n0; if (!drop[x]) { drop[x] = true; lost++; } }
	for (unsigned lost = 0; lost < 5;) { unsigned x = n0 + rnd() % n1; if (!drop[x]) { drop[x] = true; lost++; } }

	struct vrx *rx = vrx_create(S);
	struct vrx_frame out = { 0 };
	CHECK(feed_all(rx, &l, drop, 1000, &out) == 0); /* incomplete */

	struct proto_nack nacks[8];
	CHECK(vrx_nacks(rx, 1500, 5000, 0, nacks, 8) == 0);                    /* too young to complain about */
	int n = vrx_nacks(rx, 9000, 5000, 20000, nacks, 8);
	CHECK(n == 1 && nacks[0].frame_id == 20 && nacks[0].block_index == 0 && nacks[0].need == 7); /* 125 - 118 */
	CHECK(vrx_nacks(rx, 10000, 5000, 20000, nacks, 8) == 0);               /* reported recently */
	CHECK(vrx_nacks(rx, 30000, 5000, 20000, nacks, 8) == 1);               /* retried after retry_us */

	/* the sender answers with `need` repair shards, beyond the initial parity */
	struct pktlist rep = { 0 };
	for (unsigned j = 0; j < 7; j++) pl_add(&rep, &f, 0, f.blocks[0].data_shards + f.blocks[0].parity_shards + j, PROTO_PKT_FLAG_REPAIR, seq++);
	CHECK(feed_all(rx, &rep, NULL, 31000, &out) == 1);
	CHECK(out.size == 300000 && memcmp(out.data, data, 300000) == 0);
	CHECK(vrx_nacks(rx, 90000, 0, 0, nacks, 8) == 0);                      /* nothing left to ask for */
	vrx_destroy(rx);
	free(rep.buf); free(drop); free(l.buf); free(data);
}

/* A block of which nothing arrived is NACKed with VIDEO_NEED_ALL; parity shards alone then rebuild it. */
static void test_block_never_seen(void)
{
	uint64_t seq = 1;
	uint8_t *data = random_bytes(300000);
	struct vtx_frame f;
	CHECK(vtx_frame_init(&f, 30, 29, 7, 0, 0, data, 300000, S, 10) == 0);
	struct pktlist l = { 0 };
	pl_initial(&l, &f, &seq);
	bool *drop = calloc(l.n, 1);
	unsigned n0 = f.blocks[0].data_shards + f.blocks[0].parity_shards;
	for (unsigned i = n0; i < l.n; i++) drop[i] = true; /* all of block 1 is lost */

	struct vrx *rx = vrx_create(S);
	struct vrx_frame out = { 0 };
	CHECK(feed_all(rx, &l, drop, 1000, &out) == 0);
	struct proto_nack nacks[4];
	int n = vrx_nacks(rx, 50000, 5000, 20000, nacks, 4);
	CHECK(n == 1 && nacks[0].block_index == 1 && nacks[0].need == VIDEO_NEED_ALL);

	/* The sender knows the geometry and answers with k shards of any kind. Repair parity alone cannot do it for a large
	 * block: only indices k+parity..254 are free (117 here, for k = 125), so it resends some original data shards and
	 * makes up the rest with repair parity. */
	struct pktlist rep = { 0 };
	unsigned k1 = f.blocks[1].data_shards;
	for (unsigned j = 0; j < k1 / 2; j++) pl_add(&rep, &f, 1, j, PROTO_PKT_FLAG_REPAIR, seq++);
	for (unsigned j = 0; j < k1 - k1 / 2; j++) pl_add(&rep, &f, 1, k1 + f.blocks[1].parity_shards + j, PROTO_PKT_FLAG_REPAIR, seq++);
	/* and parity alone does work when the block is small enough to fit in the free indices */
	CHECK(254 - (k1 + f.blocks[1].parity_shards) + 1 < k1);
	CHECK(feed_all(rx, &rep, NULL, 51000, &out) == 1);
	CHECK(out.size == 300000 && memcmp(out.data, data, 300000) == 0);
	CHECK(vrx_stats(rx)->shards_reconstructed >= k1 - k1 / 2);
	vrx_destroy(rx);
	free(rep.buf); free(drop); free(l.buf); free(data);
}

static void test_rejects(void)
{
	uint64_t seq = 1;
	uint8_t *data = random_bytes(10000);
	struct vtx_frame f;
	CHECK(vtx_frame_init(&f, 5, 4, 77, 0, 0, data, 10000, S, 10) == 0);
	struct pktlist l = { 0 };
	pl_initial(&l, &f, &seq);
	struct vrx *rx = vrx_create(S);
	struct vrx_frame out = { 0 };

	CHECK(vrx_push(rx, l.buf, PKT, 0, &out) == 0);                          /* first packet of frame 5 */
	uint8_t bad[PKT];
	/* wrong length, wrong type */
	CHECK(vrx_push(rx, l.buf, PKT - 1, 0, &out) == -EINVAL);
	memcpy(bad, l.buf + PKT, PKT);
	bad[4] = PROTO_PKT_KEEPALIVE;
	CHECK(vrx_push(rx, bad, PKT, 0, &out) == -EINVAL);
	/* same frame id, different announced size or reference */
	struct vtx_frame g;
	CHECK(vtx_frame_init(&g, 5, 4, 77, 0, 0, data, 9000, S, 10) == 0);
	CHECK(vtx_packet(&g, SESSION, 99, 0, 1, 0, bad, PKT) == (int)PKT);
	CHECK(vrx_push(rx, bad, PKT, 0, &out) == -EPROTO);
	CHECK(vtx_frame_init(&g, 5, 3, 77, 0, 0, data, 10000, S, 10) == 0);
	CHECK(vtx_packet(&g, SESSION, 99, 0, 1, 0, bad, PKT) == (int)PKT);
	CHECK(vrx_push(rx, bad, PKT, 0, &out) == -EPROTO);
	/* inconsistent geometry within a block: different data_shards */
	CHECK(vtx_frame_init(&g, 5, 4, 77, 0, 0, data, 10000, S, 50) == 0);
	CHECK(vtx_packet(&g, SESSION, 99, 0, 2, 0, bad, PKT) == (int)PKT);
	CHECK(vrx_push(rx, bad, PKT, 0, &out) == -EPROTO);
	/* a stale epoch is ignored, not an error */
	CHECK(vtx_frame_init(&g, 6, 5, 78, 3, 0, data, 10000, S, 10) == 0);
	CHECK(vtx_packet(&g, SESSION, 99, 0, 0, 0, bad, PKT) == (int)PKT);
	CHECK(vrx_push(rx, bad, PKT, 0, &out) == 0 && vrx_stats(rx)->late == 1);
	vrx_set_epoch(rx, 3);
	CHECK(vrx_push(rx, bad, PKT, 0, &out) == 0 && vrx_stats(rx)->frames_complete == 0);
	vrx_destroy(rx);
	free(l.buf); free(data);

	/* packets that contradict the announced frame size are rejected and the frame never completes */
	rx = vrx_create(S);
	uint8_t *big = random_bytes(5000);
	struct vtx_frame h;
	CHECK(vtx_frame_init(&h, 8, 7, 1, 0, 0, big, 5000, S, 0) == 0);  /* 5 data shards */
	uint8_t pk[PKT];
	int done = 0;
	for (unsigned s = 0; s < 5; s++) {
		CHECK(vtx_packet(&h, SESSION, s, 0, s, 0, pk, PKT) == (int)PKT);
		if (s >= 3) { /* claim a frame size that needs only 3 shards while sending 5 */
			uint32_t fb = 3000;
			memcpy(pk + PROTO_PKT_HDR_SIZE + 16, &fb, 4);
		}
		int rc = vrx_push(rx, pk, PKT, 0, &out);
		if (rc == 1) done++;
	}
	CHECK(done == 0);
	vrx_destroy(rx);
	free(big);
}

static void test_window_and_late(void)
{
	struct vrx *rx = vrx_create(S);
	uint64_t seq = 1;
	uint8_t *data = random_bytes(5000);
	struct vrx_frame out = { 0 };
	/* first packet only of frames 0..99: incomplete frames are evicted as the window slides */
	for (uint32_t id = 0; id < 100; id++) {
		struct vtx_frame f;
		uint8_t pk[PKT];
		CHECK(vtx_frame_init(&f, id, id ? id - 1 : PROTO_NO_REF, id, 0, 0, data, 5000, S, 10) == 0);
		CHECK(vtx_packet(&f, SESSION, seq++, 0, 0, 0, pk, PKT) == (int)PKT);
		CHECK(vrx_push(rx, pk, PKT, id * 1000, &out) == 0);
	}
	CHECK(vrx_stats(rx)->frames_evicted == 100 - 64);
	/* a packet of an evicted, older frame is "late" */
	struct vtx_frame f;
	uint8_t pk[PKT];
	CHECK(vtx_frame_init(&f, 3, 2, 3, 0, 0, data, 5000, S, 10) == 0);
	CHECK(vtx_packet(&f, SESSION, seq++, 0, 1, 0, pk, PKT) == (int)PKT);
	CHECK(vrx_push(rx, pk, PKT, 200000, &out) == 0 && vrx_stats(rx)->late >= 1);
	/* the 64 incomplete frames still in the window are NACKed, oldest first */
	struct proto_nack nacks[100];
	int n = vrx_nacks(rx, 500000, 1000, 1000, nacks, 100);
	CHECK(n == 64 && nacks[0].frame_id == 36 && nacks[63].frame_id == 99);
	CHECK(vrx_nacks(rx, 500001, 1000, 1000, nacks, 10) == 0); /* all just reported */
	vrx_destroy(rx);
	free(data);
}

/* A statistical run over a lossy link: independent loss, then bursty loss (Gilbert-Elliott), counting frames that were
 * recovered by FEC alone. Frames are ~40 KB (34 data shards). Not strict beyond a sanity bound for the independent case. */
static void run_link(const char *name, double p_good_to_bad, double p_bad_to_good, double loss_good, double loss_bad,
		     unsigned fec, double min_fec_only)
{
	const unsigned frames = 2000;
	uint64_t seq = 1;
	struct vrx *rx = vrx_create(S);
	unsigned complete = 0, sent = 0, lost = 0;
	bool bad_state = false;
	uint8_t *data = random_bytes(40000);
	for (unsigned id = 0; id < frames; id++) {
		struct vtx_frame f;
		CHECK(vtx_frame_init(&f, id, id ? id - 1 : PROTO_NO_REF, id, 0, 0, data, 40000, S, fec) == 0);
		struct pktlist l = { 0 };
		pl_initial(&l, &f, &seq);
		struct vrx_frame out = { 0 };
		for (unsigned i = 0; i < l.n; i++) {
			if ((double)(rnd() % 1000000) / 1e6 < (bad_state ? p_bad_to_good : p_good_to_bad)) bad_state = !bad_state;
			sent++;
			if ((double)(rnd() % 1000000) / 1e6 < (bad_state ? loss_bad : loss_good)) { lost++; continue; }
			if (vrx_push(rx, l.buf + (size_t)i * PKT, PKT, id * 16000ull, &out) == 1) complete++;
		}
		free(l.buf);
	}
	double ratio = (double)complete / frames;
	printf("%-34s FEC %2u%%: %5.2f%% packets lost, %6.2f%% of frames complete without any repair\n", name, fec,
	       100.0 * lost / sent, 100.0 * ratio);
	CHECK(ratio >= min_fec_only);
	vrx_destroy(rx);
	free(data);
}

static void test_statistics(void)
{
	run_link("independent 2% loss", 0, 1, 0.02, 0.02, 10, 0.95);
	run_link("independent 2% loss", 0, 1, 0.02, 0.02, 20, 0.995);
	run_link("independent 5% loss", 0, 1, 0.05, 0.05, 20, 0.90);
	run_link("bursty (mean burst 5, ~3% loss)", 0.006, 0.2, 0.0, 0.8, 10, 0.0);
	run_link("bursty (mean burst 5, ~3% loss)", 0.006, 0.2, 0.0, 0.8, 30, 0.0);
}

/* ---- fuzz of the receiver state machine with plausible but random packets and corrupted real ones */

static void test_fuzz(void)
{
	struct vrx *rx = vrx_create(S);
	struct vrx_frame out = { 0 };
	uint8_t pk[PKT];
	uint8_t *data = random_bytes(30000);
	uint64_t seq = 1;
	for (int i = 0; i < 300000; i++) {
		if (rnd() % 4) { /* a real shard of a real frame, possibly mangled */
			struct vtx_frame f;
			unsigned size = 1 + rnd() % 30000;
			if (vtx_frame_init(&f, rnd() % 200, rnd(), rnd(), (uint8_t)(rnd() % 2), (uint8_t)(rnd() % 8), data, size, S,
					   (rnd() % 3) * 10) != 0)
				continue;
			unsigned b = rnd() % f.block_count;
			unsigned s = rnd() % (f.blocks[b].data_shards + f.blocks[b].parity_shards + 4);
			if (vtx_packet(&f, SESSION, seq++, b, s, 0, pk, PKT) != (int)PKT) continue;
			if (rnd() % 3 == 0) pk[PROTO_PKT_HDR_SIZE + rnd() % PROTO_VIDEO_HDR_SIZE] = (uint8_t)rnd(); /* corrupt a header byte */
		} else {
			for (size_t j = 0; j < PKT; j++) pk[j] = (uint8_t)rnd();
			pk[4] = PROTO_PKT_VIDEO;
		}
		(void)vrx_push(rx, pk, PKT, (uint64_t)i * 100, &out);
		if (i % 5000 == 0) {
			struct proto_nack nk[16];
			(void)vrx_nacks(rx, (uint64_t)i * 100 + 100000, 1000, 1000, nk, 16);
		}
		if (i == 150000) vrx_set_epoch(rx, 1);
	}
	vrx_destroy(rx);
	free(data);
}

int main(void)
{
	test_geometry();
	test_sizes_in_order();
	test_loss_within_parity();
	test_duplicates_and_reorder();
	test_nack_and_repair();
	test_block_never_seen();
	test_rejects();
	test_window_and_late();
	test_statistics();
	test_fuzz();
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	puts("video: all checks passed");
	return 0;
}
