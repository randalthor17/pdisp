/* The secure transport: control frames over socketpairs (with a man in the middle that tampers, drops, duplicates and reorders),
 * and datagrams (round trip, header and payload tampering, replay, window sliding, reflection). */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "net.h"
#include "proto.h"
#include "secure.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static void new_keys(uint8_t a[32], uint8_t b[32]) { pc_random(a, 32); pc_random(b, 32); }

/* ---- control channel: honest peers */

struct sender { struct net_ctl *c; const uint8_t *buf; size_t n; };
static void *send_thread(void *p) { struct sender *s = p; net_ctl_send(s->c, PROTO_CTL_CURSOR_SHAPE, s->buf, s->n); return NULL; }

static void pair(struct net_ctl *a, struct net_ctl *b, const uint8_t k1[32], const uint8_t k2[32])
{
	int sv[2];
	CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
	net_ctl_init(a, sv[0]);
	net_ctl_init(b, sv[1]);
	if (k1) { net_ctl_secure(a, k1, k2); net_ctl_secure(b, k2, k1); }
}

static void ctl_honest(void)
{
	uint8_t k1[32], k2[32];
	new_keys(k1, k2);
	struct net_ctl a, b;
	pair(&a, &b, NULL, NULL);
	uint16_t type; const uint8_t *pl; size_t plen;

	/* plain until the switch, like the handshake messages */
	CHECK(net_ctl_send(&a, PROTO_CTL_PING, (const uint8_t *)"plain", 5) == 0);
	CHECK(net_ctl_recv(&b, &type, &pl, &plen, 1000) == 1 && type == PROTO_CTL_PING && plen == 5 && memcmp(pl, "plain", 5) == 0);

	/* the sender switches first and its first secure frames arrive before the receiver has switched: the bytes must be kept */
	net_ctl_secure(&a, k1, k2);
	CHECK(net_ctl_send(&a, PROTO_CTL_PONG, (const uint8_t *)"first secure", 12) == 0);
	CHECK(net_ctl_send(&a, PROTO_CTL_PING, (const uint8_t *)"second", 6) == 0);
	nanosleep(&(struct timespec){ 0, 20000000 }, NULL);
	net_ctl_secure(&b, k2, k1);
	CHECK(net_ctl_recv(&b, &type, &pl, &plen, 1000) == 1 && type == PROTO_CTL_PONG && plen == 12 && memcmp(pl, "first secure", 12) == 0);
	CHECK(net_ctl_recv(&b, &type, &pl, &plen, 1000) == 1 && type == PROTO_CTL_PING && plen == 6);
	net_ctl_close(&a);
	net_ctl_close(&b);

	/* sizes from empty to larger than any socket buffer, both directions, on one pair so the counters keep running */
	pair(&a, &b, k1, k2);
	size_t sizes[] = { 0, 1, 15, 16, 17, 1000, 70000, 600000 };
	for (int round = 0; round < 2; round++)
		for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
			uint8_t *buf = malloc(sizes[i] + 1);
			for (size_t j = 0; j < sizes[i]; j++) buf[j] = (uint8_t)(j * 31 + i);
			int forward = (round + (int)i) % 2;
			struct net_ctl *from = forward ? &a : &b, *to = forward ? &b : &a;
			struct sender s = { from, buf, sizes[i] };
			pthread_t t;
			pthread_create(&t, NULL, send_thread, &s);
			int r = net_ctl_recv(to, &type, &pl, &plen, 5000);
			CHECK(r == 1 && type == PROTO_CTL_CURSOR_SHAPE && plen == sizes[i] && (sizes[i] == 0 || memcmp(pl, buf, sizes[i]) == 0));
			pthread_join(t, NULL);
			free(buf);
		}
	/* a 1 MiB frame is the limit; the sender refuses anything larger */
	uint8_t *big = calloc(1, PROTO_MAX_CTL_PAYLOAD + 1);
	CHECK(net_ctl_send(&a, PROTO_CTL_CURSOR_SHAPE, big, PROTO_MAX_CTL_PAYLOAD + 1) < 0);
	free(big);
	net_ctl_close(&a);
	net_ctl_close(&b);
}

/* ---- control channel: a man in the middle */

#define NFRAMES 4
struct stream { uint8_t frame[NFRAMES][64]; size_t len[NFRAMES]; };

/* A sends NFRAMES small frames into a byte pipe the test controls; returns them split into secure frames. */
static void capture(struct stream *s, const uint8_t k1[32], const uint8_t k2[32])
{
	int sv[2];
	CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
	struct net_ctl a;
	net_ctl_init(&a, sv[0]);
	net_ctl_secure(&a, k1, k2);
	for (int i = 0; i < NFRAMES; i++) {
		uint8_t msg[8];
		memset(msg, 'a' + i, sizeof msg);
		CHECK(net_ctl_send(&a, PROTO_CTL_PING, msg, sizeof msg) == 0);
	}
	for (int i = 0; i < NFRAMES; i++) {
		uint8_t h[4];
		CHECK(recv(sv[1], h, 4, MSG_WAITALL) == 4);
		size_t clen = h[0] | h[1] << 8 | h[2] << 16 | (size_t)h[3] << 24;
		memcpy(s->frame[i], h, 4);
		CHECK(recv(sv[1], s->frame[i] + 4, clen, MSG_WAITALL) == (ssize_t)clen);
		s->len[i] = 4 + clen;
	}
	net_ctl_close(&a);
	close(sv[1]);
}

/* feeds the chosen bytes to a fresh receiver and returns how many frames it accepted before the first error (in *first_error) */
static int deliver(const uint8_t *bytes, size_t n, const uint8_t k1[32], const uint8_t k2[32], int *first_error, int close_after)
{
	int sv[2];
	CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
	struct net_ctl b;
	net_ctl_init(&b, sv[1]);
	net_ctl_secure(&b, k2, k1);
	CHECK(send(sv[0], bytes, n, 0) == (ssize_t)n);
	if (close_after) close(sv[0]);
	int ok = 0;
	*first_error = 0;
	for (;;) {
		uint16_t type; const uint8_t *pl; size_t plen;
		int r = net_ctl_recv(&b, &type, &pl, &plen, 300);
		if (r == 1) { ok++; continue; }
		*first_error = r; /* 0: nothing more arrived (waiting), negative: the connection must be dropped */
		break;
	}
	net_ctl_close(&b);
	if (!close_after) close(sv[0]);
	return ok;
}

static size_t join(uint8_t *out, const struct stream *s, const int *order, int count)
{
	size_t n = 0;
	for (int i = 0; i < count; i++) { memcpy(out + n, s->frame[order[i]], s->len[order[i]]); n += s->len[order[i]]; }
	return n;
}

static void ctl_attacks(void)
{
	uint8_t k1[32], k2[32];
	new_keys(k1, k2);
	struct stream s;
	capture(&s, k1, k2);
	uint8_t buf[NFRAMES * 64];
	int err;

	int all[] = { 0, 1, 2, 3 };
	size_t n = join(buf, &s, all, 4);
	CHECK(deliver(buf, n, k1, k2, &err, 0) == 4 && err == 0);

	/* flipping any bit of the ciphertext or tag of frame 1 is caught after frame 0 was delivered; the length bytes are covered too */
	int caught = 0, tried = 0;
	for (size_t pos = 4; pos < s.len[1]; pos++) {
		uint8_t t[NFRAMES * 64];
		memcpy(t, buf, n);
		t[s.len[0] + pos] ^= 0x01;
		tried++;
		int ok = deliver(t, n, k1, k2, &err, 0);
		if (ok == 1 && err < 0) caught++;
	}
	CHECK(caught == tried);
	uint8_t t[NFRAMES * 64];
	memcpy(t, buf, n);
	t[s.len[0]] ^= 0x01; /* the length: the frame no longer parses as the same frame (rejected or never completes) */
	int ok = deliver(t, n, k1, k2, &err, 0);
	CHECK(ok == 1 && err <= 0 && !(ok == 4));

	int dropped[] = { 0, 2, 3 };           /* a frame missing */
	n = join(t, &s, dropped, 3);
	ok = deliver(t, n, k1, k2, &err, 0);
	CHECK(ok == 1 && err < 0);
	int dup[] = { 0, 1, 1, 2 };            /* a frame repeated */
	n = join(t, &s, dup, 4);
	ok = deliver(t, n, k1, k2, &err, 0);
	CHECK(ok == 2 && err < 0);
	int swapped[] = { 0, 2, 1, 3 };        /* two frames swapped */
	n = join(t, &s, swapped, 4);
	ok = deliver(t, n, k1, k2, &err, 0);
	CHECK(ok == 1 && err < 0);
	int replay_first[] = { 1, 0 };         /* starting somewhere else: the first frame must be frame 0 */
	n = join(t, &s, replay_first, 2);
	ok = deliver(t, n, k1, k2, &err, 0);
	CHECK(ok == 0 && err < 0);

	/* cut in the middle of a frame, then the connection closes: nothing partial is delivered */
	n = join(t, &s, all, 4);
	ok = deliver(t, n - 5, k1, k2, &err, 1);
	CHECK(ok == 3 && err == -ECONNRESET);

	/* frames under another key, a plain frame, a huge length */
	uint8_t o1[32], o2[32];
	new_keys(o1, o2);
	struct stream other;
	capture(&other, o1, o2);
	n = join(t, &other, all, 4);
	ok = deliver(t, n, k1, k2, &err, 0);
	CHECK(ok == 0 && err < 0);
	uint8_t plainframe[64];
	int pn = proto_ctl_frame_write(PROTO_CTL_PING, (const uint8_t *)"hijack attempt!!", 16, plainframe, sizeof plainframe);
	ok = deliver(plainframe, (size_t)pn, k1, k2, &err, 0);
	CHECK(ok == 0 && err < 0);
	uint8_t huge[8] = { 0xff, 0xff, 0xff, 0x7f, 1, 2, 3, 4 };
	ok = deliver(huge, sizeof huge, k1, k2, &err, 0);
	CHECK(ok == 0 && err == -EMSGSIZE);
	uint8_t tiny[8] = { 3, 0, 0, 0, 1, 2, 3, 4 }; /* shorter than a tag */
	ok = deliver(tiny, sizeof tiny, k1, k2, &err, 0);
	CHECK(ok == 0 && err == -EMSGSIZE);
}

/* ---- datagrams */

static void make_pkt(uint8_t *p, size_t payload, uint64_t seq, uint8_t type)
{
	struct proto_pkt_hdr h = { .session_id = 0x01020304, .type = type, .flags = 0, .seq = seq };
	proto_pkt_hdr_write(&h, p, 16);
	for (size_t i = 0; i < payload; i++) p[16 + i] = (uint8_t)(i * 13 + seq);
}

static void udp_basic(void)
{
	uint8_t k1[32], k2[32];
	new_keys(k1, k2);
	struct sec_udp phone, pc;
	sec_udp_init(&phone, k1, k2, 1); /* the phone sends with k1, the PC with k2 */
	sec_udp_init(&pc, k2, k1, 0);
	uint8_t pkt[1500], ref[1500];

	for (size_t payload = 0; payload <= 1300; payload += (payload < 40 ? 1 : 157)) {
		make_pkt(pkt, payload, 1000 + payload, PROTO_PKT_VIDEO);
		memcpy(ref, pkt, 16 + payload);
		int n = sec_udp_seal(&pc, pkt, 16 + payload, sizeof pkt);
		CHECK(n == (int)(16 + payload + 16));
		CHECK(memcmp(pkt, ref, 16) == 0);                       /* the header stays readable */
		CHECK(payload < 8 || memcmp(pkt + 16, ref + 16, payload) != 0); /* the payload does not */
		int m = sec_udp_open(&phone, pkt, (size_t)n);
		CHECK(m == (int)(16 + payload) && memcmp(pkt, ref, (size_t)m) == 0);
	}

	/* every single-bit change anywhere in a sealed packet is rejected, and a rejected packet does not move the window */
	make_pkt(pkt, 100, 5000, PROTO_PKT_VIDEO);
	int n = sec_udp_seal(&pc, pkt, 116, sizeof pkt);
	int rejected = 0, tried = 0;
	for (int pos = 0; pos < n; pos++) {
		uint8_t t[1500];
		memcpy(t, pkt, (size_t)n);
		t[pos] ^= 0x10;
		tried++;
		struct sec_udp rx = phone; /* a copy, so each trial starts from the same window */
		if (sec_udp_open(&rx, t, (size_t)n) < 0) rejected++;
	}
	CHECK(rejected == tried);
	uint8_t tt[1500];
	memcpy(tt, pkt, (size_t)n);
	tt[8] ^= 0x01; /* a seq above the window, forged: rejected and must not advance the window */
	tt[15] = 0x7f;
	CHECK(sec_udp_open(&phone, tt, (size_t)n) == -EBADMSG);
	memcpy(tt, pkt, (size_t)n);
	CHECK(sec_udp_open(&phone, tt, (size_t)n) == 116); /* the genuine one still opens */
	memcpy(tt, pkt, (size_t)n);
	CHECK(sec_udp_open(&phone, tt, (size_t)n) == -EEXIST); /* and only once */

	/* too short, and the wrong direction (a packet the PC sent cannot be accepted by the PC: reflection) */
	CHECK(sec_udp_open(&phone, pkt, 20) == -EINVAL);
	make_pkt(pkt, 50, 6000, PROTO_PKT_FEEDBACK);
	n = sec_udp_seal(&pc, pkt, 66, sizeof pkt);
	CHECK(sec_udp_open(&pc, pkt, (size_t)n) == -EBADMSG);
	CHECK(sec_udp_seal(&pc, pkt, 10, sizeof pkt) == -EINVAL && sec_udp_seal(&pc, pkt, 66, 70) == -ENOSPC);
}

/* a packet with this sequence number, sealed by `tx` and handed to `rx` */
static int via(struct sec_udp *tx, struct sec_udp *rx, uint64_t seq)
{
	uint8_t pkt[200];
	make_pkt(pkt, 20, seq, PROTO_PKT_VIDEO);
	int n = sec_udp_seal(tx, pkt, 36, sizeof pkt);
	return sec_udp_open(rx, pkt, (size_t)n);
}

static void udp_epochs(void)
{
	uint8_t k1[32], k2[32];
	new_keys(k1, k2);
	struct sec_udp tx, rx;
	sec_udp_init(&tx, k1, k2, 1);
	sec_udp_init(&rx, k2, k1, 0);
	uint8_t a[100], b[100];
	/* the same sequence number in two epochs must never be encrypted with the same keystream */
	make_pkt(a, 40, 7, PROTO_PKT_VIDEO);
	memcpy(b, a, 56);
	int na = sec_udp_seal(&tx, a, 56, sizeof a);
	sec_udp_set_epoch(&tx, 1);
	int nb = sec_udp_seal(&tx, b, 56, sizeof b);
	CHECK(na == nb && memcmp(a + 16, b + 16, 56 - 16 + 16) != 0);
	/* the receiver in epoch 0 accepts the first and refuses the second; after switching it is the other way round */
	uint8_t a2[100], b2[100];
	memcpy(a2, a, (size_t)na); memcpy(b2, b, (size_t)nb);
	CHECK(sec_udp_open(&rx, a, (size_t)na) == 56);
	CHECK(sec_udp_open(&rx, b, (size_t)nb) < 0); /* (already-seen sequence number: refused before any cryptography) */
	sec_udp_set_epoch(&rx, 1);
	CHECK(sec_udp_open(&rx, a2, (size_t)na) == -EBADMSG);
	CHECK(sec_udp_open(&rx, b2, (size_t)nb) == 56);
	/* the new epoch starts a fresh window: sequence numbers start again from 0 and are accepted once each */
	CHECK(via(&tx, &rx, 0) == 36 && via(&tx, &rx, 0) == -EEXIST && via(&tx, &rx, 1) == 36);
	/* epochs are independent, not a chain: going to epoch 0 again would reuse keys, which the callers never do, but it must at least
	 * give the epoch-0 keys back deterministically */
	struct sec_udp again;
	sec_udp_init(&again, k1, k2, 1);
	CHECK(memcmp(again.send_key, tx.send_key, 32) != 0);
	sec_udp_set_epoch(&again, 1);
	CHECK(memcmp(again.send_key, tx.send_key, 32) == 0);
	CHECK(memcmp(again.send_key, again.recv_key, 32) != 0 && memcmp(again.send_key, k1, 32) != 0);
}

static void udp_window(void)
{
	uint8_t k1[32], k2[32];
	new_keys(k1, k2);
	struct sec_udp tx, rx;
	sec_udp_init(&tx, k1, k2, 1);
	sec_udp_init(&rx, k2, k1, 0);
	uint8_t pkt[200];
	(void)pkt;
	#define SEND(seq) via(&tx, &rx, (seq))
	CHECK(SEND(10) == 36);
	CHECK(SEND(10) == -EEXIST);
	CHECK(SEND(9) == 36);              /* reordered but new: accepted */
	CHECK(SEND(9) == -EEXIST);
	CHECK(SEND(5000) == 36);           /* a big jump slides the window */
	CHECK(SEND(10) == -EEXIST);        /* now far older than the window: rejected, whether or not it was seen */
	CHECK(SEND(11) == -EEXIST);
	CHECK(SEND(5000 - 1023) == 36);    /* the oldest slot still inside the window */
	CHECK(SEND(5000 - 1024) == -EEXIST);
	CHECK(SEND(4999) == 36);
	CHECK(SEND(4999) == -EEXIST);
	CHECK(SEND(5001) == 36 && SEND(5000) == -EEXIST && SEND(5001) == -EEXIST);
	/* a shift by exactly 64, 65 and 1024 bits keeps the bookkeeping right */
	CHECK(SEND(5001 + 64) == 36 && SEND(5001) == -EEXIST && SEND(5001 + 63) == 36 && SEND(5001 + 63) == -EEXIST);
	CHECK(SEND(5001 + 64 + 65) == 36 && SEND(5001 + 64) == -EEXIST && SEND(5001 + 64 + 64) == 36);
	CHECK(SEND(5001 + 64 + 65 + 1024) == 36 && SEND(5001 + 64 + 65) == -EEXIST && SEND(5001 + 64 + 65 + 1) == 36);
	/* heavy shuffling: every sequence number is accepted exactly once */
	struct sec_udp tx2, rx2;
	sec_udp_init(&tx2, k1, k2, 1);
	sec_udp_init(&rx2, k2, k1, 0);
	int accepted = 0, dups = 0;
	for (int round = 0; round < 3; round++)
		for (int i = 0; i < 2000; i++) {
			uint64_t seq = (uint64_t)(i / 8) * 8 + (7 - i % 8) + 100000; /* groups of 8 in reverse order */
			make_pkt(pkt, 10, seq, PROTO_PKT_VIDEO);
			int n = sec_udp_seal(&tx2, pkt, 26, sizeof pkt);
			int r = sec_udp_open(&rx2, pkt, (size_t)n);
			if (r > 0) accepted++; else if (r == -EEXIST) dups++;
		}
	CHECK(accepted == 2000 && dups == 4000);
}

int main(void)
{
	ctl_honest();
	ctl_attacks();
	udp_basic();
	udp_window();
	udp_epochs();
	if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
	puts("secure: all checks passed");
	return 0;
}
