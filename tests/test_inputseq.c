/* The input event stream over a lossy, duplicating, reordering link, with a model of what the PC's touch screen would show. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "inputseq.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)(rng >> 16); }
static int chance(int percent) { return (int)(rnd() % 100) < percent; }

static struct proto_input_event ev(uint8_t kind, uint8_t id, uint16_t x, uint16_t y)
{
	struct proto_input_event e;
	memset(&e, 0, sizeof e);
	e.kind = kind; e.pointer_id = id; e.x = x; e.y = y; e.pressure = 1000;
	return e;
}

/* what the PC would apply: for every finger, down/up state and last position, in the order applied */
struct screen { int down[10], ignore[10]; uint16_t x[10], y[10]; uint32_t last_seq; uint64_t applied; int order_ok; int bad_transition; };

static void screen_apply(struct screen *s, const struct proto_input_event *e)
{
	if (e->seq != s->last_seq + 1 && s->last_seq) s->order_ok = 0;
	s->last_seq = e->seq;
	s->applied++;
	int id = e->pointer_id;
	if (e->kind != PROTO_IN_DOWN && s->ignore[id]) return; /* a finger the PC released after a gap: its remaining events mean nothing */
	if (e->kind == PROTO_IN_DOWN) s->ignore[id] = 0;
	if (e->kind == PROTO_IN_DOWN) { if (s->down[id]) s->bad_transition++; s->down[id] = 1; }
	else if (e->kind == PROTO_IN_UP || e->kind == PROTO_IN_CANCEL) { if (!s->down[id]) s->bad_transition++; s->down[id] = 0; }
	else if (!s->down[id]) s->bad_transition++; /* a move for a finger that is not down */
	s->x[id] = e->x; s->y[id] = e->y;
}

/* a scripted gesture generator: two fingers touch, drag, lift, repeatedly */
struct gen { int phase, count; };
static int gen_next(struct gen *g, struct proto_input_event *out)
{
	static const uint8_t script[] = { 0, 1, 2, 2, 2, 2, 3, 4 }; /* down 0, down 1, moves, up 0, up 1 */
	int s = script[g->phase % sizeof script];
	g->phase++;
	g->count++;
	switch (s) {
	case 0: *out = ev(PROTO_IN_DOWN, 0, 1000, 1000); return 1;
	case 1: *out = ev(PROTO_IN_DOWN, 1, 30000, 30000); return 1;
	case 2: *out = ev(PROTO_IN_MOVE, (uint8_t)(g->count & 1), (uint16_t)(g->count * 7), (uint16_t)(g->count * 3)); return 1;
	case 3: *out = ev(PROTO_IN_UP, 0, 0, 0); return 1;
	default: *out = ev(PROTO_IN_UP, 1, 0, 0); return 1;
	}
}

/* runs the whole thing: `events` events generated one per 5 ms tick, packets and acks lost with the given percentages, packets
 * sometimes duplicated and delayed. Returns the screen the PC ended with. */
static void run(int events, int loss_pct, int ack_loss_pct, int outage_from, int outage_to, struct screen *screen, struct input_tx *tx,
		struct input_rx *rx, int *gaps)
{
	input_tx_init(tx);
	input_rx_init(rx);
	memset(screen, 0, sizeof *screen);
	screen->order_ok = 1;
	*gaps = 0;
	struct gen g = { 0, 0 };
	struct { uint8_t data[1024]; int len; uint64_t due; } inflight[64];
	int nin = 0;
	struct { uint32_t acked; uint64_t due; } acks[64];
	int nack = 0;
	uint64_t now = 0;
	int generated = 0;
	for (int tick = 0; tick < events * 4 + 2000; tick++, now += 5000) {
		if (generated < events) {
			struct proto_input_event e;
			gen_next(&g, &e);
			input_tx_push(tx, e);
			generated++;
		}
		uint8_t buf[1024];
		int n = input_tx_build(tx, now, 15000, buf, sizeof buf);
		int blackout = tick >= outage_from && tick < outage_to;
		if (n > 0 && !blackout && !chance(loss_pct) && nin < 64) {
			inflight[nin].len = n;
			memcpy(inflight[nin].data, buf, (size_t)n);
			inflight[nin].due = now + (uint64_t)(rnd() % 40000); /* up to 40 ms: packets overtake each other */
			nin++;
			if (chance(10) && nin < 64) { inflight[nin] = inflight[nin - 1]; inflight[nin].due += (uint64_t)(rnd() % 60000); nin++; } /* a duplicate */
		}
		for (int i = 0; i < nin;) {
			if (inflight[i].due > now) { i++; continue; }
			struct proto_input in;
			CHECK(proto_input_decode(inflight[i].data, (size_t)inflight[i].len, &in) == 0);
			struct proto_input_event out[PROTO_MAX_INPUT_EVENTS];
			int gap;
			unsigned k = input_rx_accept(rx, &in, out, &gap);
			if (gap) { /* release everything the PC thinks is down, and ignore those fingers until they touch down again */
				(*gaps)++;
				for (int f = 0; f < 10; f++) { if (screen->down[f]) screen->ignore[f] = 1; screen->down[f] = 0; }
				screen->last_seq = 0;
			}
			for (unsigned j = 0; j < k; j++) screen_apply(screen, &out[j]);
			if (nack < 64 && !chance(ack_loss_pct)) { acks[nack].acked = input_rx_acked(rx); acks[nack].due = now + (uint64_t)(rnd() % 30000); nack++; }
			inflight[i] = inflight[--nin];
		}
		for (int i = 0; i < nack;) {
			if (acks[i].due > now) { i++; continue; }
			input_tx_ack(tx, acks[i].acked);
			acks[i] = acks[--nack];
		}
	}
}

static void basics(void)
{
	struct input_tx t;
	struct input_rx r;
	input_tx_init(&t);
	input_rx_init(&r);
	uint8_t buf[1024];
	CHECK(input_tx_build(&t, 0, 10000, buf, sizeof buf) == 0); /* nothing to send */
	input_tx_push(&t, ev(PROTO_IN_DOWN, 0, 100, 200));
	input_tx_push(&t, ev(PROTO_IN_MOVE, 0, 110, 210));
	int n = input_tx_build(&t, 1000, 10000, buf, sizeof buf);
	CHECK(n > 0);
	CHECK(input_tx_build(&t, 2000, 10000, buf, sizeof buf) == 0);     /* sent just now, nothing new */
	CHECK(input_tx_build(&t, 12000, 10000, buf, sizeof buf) > 0);     /* unacknowledged: repeated after the interval */
	struct proto_input in;
	CHECK(proto_input_decode(buf, (size_t)n, &in) == 0 && in.count == 2 && in.events[0].seq == 1 && in.events[1].seq == 2);
	struct proto_input_event out[32];
	int gap;
	CHECK(input_rx_accept(&r, &in, out, &gap) == 2 && !gap && input_rx_acked(&r) == 2);
	CHECK(input_rx_accept(&r, &in, out, &gap) == 0 && !gap && r.duplicates == 2); /* the same packet again applies nothing */
	input_tx_ack(&t, 1);
	CHECK(input_tx_pending(&t) == 1);
	input_tx_ack(&t, 2);
	CHECK(input_tx_pending(&t) == 0 && input_tx_build(&t, 99000, 10000, buf, sizeof buf) == 0);
	input_tx_ack(&t, 1); /* a late duplicate ack changes nothing */
	CHECK(input_tx_pending(&t) == 0);
	/* sequence numbers wrap cleanly */
	struct input_rx w;
	input_rx_init(&w);
	w.next_seq = 0xfffffffe;
	struct proto_input win;
	memset(&win, 0, sizeof win);
	win.count = 3;
	for (int i = 0; i < 3; i++) { win.events[i] = ev(PROTO_IN_MOVE, 0, 1, 1); win.events[i].seq = 0xfffffffe + (uint32_t)i; }
	CHECK(input_rx_accept(&w, &win, out, &gap) == 3 && !gap && w.next_seq == 1);
}

static void lossy(void)
{
	static const int losses[] = { 0, 10, 30, 50, 70, 85 };
	for (unsigned l = 0; l < sizeof losses / sizeof losses[0]; l++) {
		struct screen s;
		struct input_tx tx;
		struct input_rx rx;
		int gaps;
		run(400, losses[l], losses[l], -1, -1, &s, &tx, &rx, &gaps);
		/* every event is applied exactly once and in order, and the PC's idea of the fingers is the sender's: none left down */
		if (losses[l] <= 70) { /* a lossy link: everything arrives exactly once, in order */
			CHECK(s.applied == 400);
			CHECK(s.order_ok && s.bad_transition == 0 && gaps == 0 && tx.dropped == 0);
			CHECK(input_rx_acked(&rx) == 400 && input_tx_pending(&tx) == 0);
		} else { /* an extreme one overflows the queue: events are lost for good, which the receiver must notice and survive */
			CHECK(s.bad_transition == 0 && gaps == (int)rx.gaps && tx.dropped > 0);
		}
		CHECK(!s.down[0] && !s.down[1]); /* in every case no finger is left down */
	}
}

static void outage(void)
{
	/* a blackout of 3 seconds (600 ticks of 5 ms) in the middle of a drag: the queue overflows; moves are coalesced, downs and ups survive,
	 * so the PC never sees a finger stuck down */
	struct screen s;
	struct input_tx tx;
	struct input_rx rx;
	int gaps;
	run(800, 20, 20, 300, 900, &s, &tx, &rx, &gaps);
	CHECK(tx.coalesced > 0);
	CHECK(!s.down[0] && !s.down[1]);
	CHECK(s.bad_transition == 0);
	/* the same with a script of only downs and ups: nothing can be coalesced, so events are dropped, the receiver sees the gap and
	 * releases everything: still no stuck finger */
	struct input_tx t2;
	struct input_rx r2;
	input_tx_init(&t2);
	input_rx_init(&r2);
	for (int i = 0; i < 40; i++) input_tx_push(&t2, ev(i % 2 ? PROTO_IN_UP : PROTO_IN_DOWN, 3, 5, 5)); /* 40 > 32 with no acknowledgement */
	CHECK(t2.dropped == 8 && input_tx_pending(&t2) == 32 && t2.lost_upto == 8);
	uint8_t buf[1024];
	int n = input_tx_build(&t2, 0, 1, buf, sizeof buf);
	struct proto_input in;
	CHECK(proto_input_decode(buf, (size_t)n, &in) == 0);
	struct proto_input_event out[32];
	int gap = 0;
	CHECK(in.has_lost && in.lost_upto == 8);
	CHECK(input_rx_accept(&r2, &in, out, &gap) == 32 && gap == 1 && r2.gaps == 1);
	CHECK(input_rx_accept(&r2, &in, out, &gap) == 0 && gap == 0); /* the same packet again: not a new loss */
	/* once acknowledged past the loss, the packets stop reporting it */
	input_tx_ack(&t2, 20);
	n = input_tx_build(&t2, 100000, 1, buf, sizeof buf);
	CHECK(proto_input_decode(buf, (size_t)n, &in) == 0 && !in.has_lost);
}

static void coalescing_keeps_the_ends(void)
{
	struct input_tx t;
	input_tx_init(&t);
	input_tx_push(&t, ev(PROTO_IN_DOWN, 0, 1, 1));
	for (int i = 0; i < 60; i++) input_tx_push(&t, ev(PROTO_IN_MOVE, 0, (uint16_t)(10 + i), 5)); /* far more than fit */
	input_tx_push(&t, ev(PROTO_IN_UP, 0, 99, 5));
	CHECK(t.dropped == 0 && t.coalesced > 0 && input_tx_pending(&t) == 32 && t.lost_upto == 0); /* coalescing is not a loss */
	CHECK(t.q[0].kind == PROTO_IN_DOWN);                  /* the first and the last events survive... */
	CHECK(t.q[t.n - 1].kind == PROTO_IN_UP);
	CHECK(t.q[t.n - 2].kind == PROTO_IN_MOVE && t.q[t.n - 2].x == 10 + 59); /* ...and the newest position */
	for (unsigned i = 1; i < t.n; i++) CHECK(t.q[i].seq > t.q[i - 1].seq);
}

int main(void)
{
	basics();
	lossy();
	outage();
	coalescing_keeps_the_ends();
	if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
	puts("inputseq: all checks passed");
	return 0;
}
