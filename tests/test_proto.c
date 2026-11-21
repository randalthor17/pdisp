/* Tests for the wire format (proto/proto.c): byte-exact vectors computed by hand from docs/protocol.md, round trips,
 * truncation at every length, TLV skipping, limits, and a deterministic mutation fuzz (meant to run under ASan/UBSan). */
#include "proto.h"

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

static int bytes_equal(const uint8_t *got, int got_len, const uint8_t *want, size_t want_len)
{
	if (got_len < 0 || (size_t)got_len != want_len || memcmp(got, want, want_len) != 0) {
		fprintf(stderr, "  got %d bytes:", got_len);
		for (int i = 0; i < got_len && i < 64; i++) fprintf(stderr, " %02x", got[i]);
		fprintf(stderr, "\n");
		return 0;
	}
	return 1;
}

/* ---- golden vectors */

static void test_vectors(void)
{
	uint8_t b[128];

	/* common header: session 0x11223344, VIDEO, no flags, seq 1 (the example in section 3.1) */
	struct proto_pkt_hdr h = { .session_id = 0x11223344, .type = PROTO_PKT_VIDEO, .flags = 0, .seq = 1 };
	static const uint8_t want_hdr[16] = { 0x44, 0x33, 0x22, 0x11, 0x01, 0x00, 0x00, 0x00,
					      0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
	CHECK(bytes_equal(b, proto_pkt_hdr_write(&h, b, sizeof b), want_hdr, sizeof want_hdr));
	struct proto_pkt_hdr h2;
	CHECK(proto_pkt_hdr_read(want_hdr, sizeof want_hdr, &h2) == 0 && h2.session_id == 0x11223344 && h2.seq == 1 &&
	      h2.type == PROTO_PKT_VIDEO);

	/* PING frame: length 12, type 0x0030, flags 0, token 0x0102030405060708 */
	uint8_t tok[8], frame[32];
	CHECK(proto_token_encode(0x0102030405060708ull, tok, sizeof tok) == 8);
	static const uint8_t want_ping[16] = { 0x0c, 0x00, 0x00, 0x00, 0x30, 0x00, 0x00, 0x00,
					       0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01 };
	CHECK(bytes_equal(frame, proto_ctl_frame_write(PROTO_CTL_PING, tok, 8, frame, sizeof frame), want_ping, sizeof want_ping));

	/* KEEPALIVE: newest 16, now_us 0x100, input_acked 2 */
	struct proto_keepalive ka = { .newest_frame_id = 16, .now_us = 0x100, .input_acked = 2 };
	static const uint8_t want_ka[16] = { 0x10, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
					     0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00 };
	CHECK(bytes_equal(b, proto_keepalive_encode(&ka, b, sizeof b), want_ka, sizeof want_ka));

	/* VIDEO header: frame 5 predicting from 4, captured at 1000 us, sent at 2000 us, 3000 bytes, epoch 1, block 0 of 1, shard 2 of 3+1, IDR */
	struct proto_video_hdr v = { .frame_id = 5, .ref_frame_id = 4, .capture_us = 1000, .send_us = 2000, .frame_bytes = 3000, .epoch = 1,
				     .block_index = 0, .block_count = 1, .shard_index = 2, .data_shards = 3,
				     .parity_shards = 1, .vflags = PROTO_VFLAG_IDR };
	static const uint8_t want_v[32] = { 0x05, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0xe8, 0x03, 0x00, 0x00,
					    0x00, 0x00, 0x00, 0x00, 0xd0, 0x07, 0x00, 0x00, 0xb8, 0x0b, 0x00, 0x00,
					    0x01, 0x00, 0x01, 0x02, 0x03, 0x01, 0x01, 0x00 };
	CHECK(bytes_equal(b, proto_video_hdr_write(&v, b, sizeof b), want_v, sizeof want_v));

	/* INPUT with one event: seq 7, down, pointer 1, finger, x 0x8000, y 0x4000, pressure 0xffff, tilt -1/5, t 0x1000 */
	struct proto_input in = { .count = 1 };
	in.events[0] = (struct proto_input_event){ .seq = 7, .kind = PROTO_IN_DOWN, .pointer_id = 1, .tool = PROTO_TOOL_FINGER,
						   .x = 0x8000, .y = 0x4000, .pressure = 0xffff, .tilt_x = -1, .tilt_y = 5,
						   .time_us = 0x1000 };
	static const uint8_t want_in[21] = { 0x01, 0x07, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x80,
					     0x00, 0x40, 0xff, 0xff, 0xff, 0x05, 0x00, 0x10, 0x00, 0x00 };
	CHECK(bytes_equal(b, proto_input_encode(&in, b, sizeof b), want_in, sizeof want_in));
	/* the same with a loss marker: bit 7 of the count byte, and the u32 lost_upto after the events */
	in.has_lost = 1;
	in.lost_upto = 0x01020304;
	uint8_t lostb[40];
	int ln = proto_input_encode(&in, lostb, sizeof lostb);
	CHECK(ln == 25 && lostb[0] == 0x81 && memcmp(lostb + 1, want_in + 1, 20) == 0 && lostb[21] == 0x04 && lostb[22] == 0x03 &&
	      lostb[23] == 0x02 && lostb[24] == 0x01);
	struct proto_input back;
	CHECK(proto_input_decode(lostb, 25, &back) == 0 && back.count == 1 && back.has_lost && back.lost_upto == 0x01020304);
	CHECK(proto_input_decode(lostb, 24, &back) != 0); /* a marker without its number is cut short */
	in.has_lost = 0;
	in.lost_upto = 0;

	/* REJECT: reason 3, text "busy": u16 3, u16 4, "busy" */
	struct proto_reject rj = { .reason = 3 };
	snprintf(rj.text, sizeof rj.text, "busy");
	static const uint8_t want_rj[8] = { 0x03, 0x00, 0x04, 0x00, 'b', 'u', 's', 'y' };
	CHECK(bytes_equal(b, proto_reject_encode(&rj, b, sizeof b), want_rj, sizeof want_rj));

	/* CURSOR_POS: shape 2, x 0x1234, y 0xabcd, move_seq 9 */
	struct proto_cursor_pos cp = { .shape_id = 2, .x = 0x1234, .y = 0xabcd, .move_seq = 9 };
	static const uint8_t want_cp[10] = { 0x02, 0x00, 0x34, 0x12, 0xcd, 0xab, 0x09, 0x00, 0x00, 0x00 };
	CHECK(bytes_equal(b, proto_cursor_pos_encode(&cp, b, sizeof b), want_cp, sizeof want_cp));
}

/* ---- sample messages */

static struct proto_hello sample_hello(void)
{
	struct proto_hello m;
	memset(&m, 0, sizeof m);
	m.proto_min = 1;
	m.proto_max = 2;
	snprintf(m.device_name, sizeof m.device_name, "Galaxy Tab S9 FE");
	m.device_caps = PROTO_CAP_TOUCH | PROTO_CAP_STYLUS;
	m.native_width = 2304;
	m.native_height = 1440;
	m.density_dpi = 262;
	m.orientation = 0;
	m.inset_top = 24;
	m.n_refresh = 3;
	m.refresh_mhz[0] = 60000; m.refresh_mhz[1] = 90000; m.refresh_mhz[2] = 120000;
	m.n_codecs = 1;
	m.codecs[0] = PROTO_CODEC_HEVC;
	m.max_touch_points = 10;
	m.max_udp_payload = 1472;
	snprintf(m.app_version, sizeof m.app_version, "0.1.0");
	return m;
}

static struct proto_setup sample_setup(void)
{
	struct proto_setup m;
	memset(&m, 0, sizeof m);
	m.proto_version = 1;
	m.session_id = 0xdeadbeef;
	m.udp_port = 47110;
	m.params = (struct proto_stream_params){ .epoch = 3, .security = PROTO_SECURITY_NONE, .width = 2304, .height = 1440,
						 .refresh_mhz = 90000, .codec = PROTO_CODEC_HEVC, .color_range = 0,
						 .color_primaries = 1, .color_transfer = 13, .color_matrix = 1, .shard_size = 1200,
						 .fec_percent = 10, .bitrate_kbps = 20000, .min_bitrate_kbps = 2000,
						 .max_bitrate_kbps = 40000, .dpb_frames = 7, .param_sets_len = 100 };
	for (int i = 0; i < 100; i++) m.params.param_sets[i] = (uint8_t)(i * 7);
	return m;
}

static void test_roundtrips(void)
{
	uint8_t b[4096];
	int n;

	struct proto_hello h = sample_hello(), h2;
	n = proto_hello_encode(&h, b, sizeof b);
	CHECK(n > 0 && proto_hello_decode(b, (size_t)n, &h2) == 0 && memcmp(&h, &h2, sizeof h) == 0);

	struct proto_setup s = sample_setup(), s2;
	n = proto_setup_encode(&s, b, sizeof b);
	CHECK(n > 0 && proto_setup_decode(b, (size_t)n, &s2) == 0 && memcmp(&s, &s2, sizeof s) == 0);

	struct proto_stream_params p2;
	n = proto_stream_params_encode(&s.params, b, sizeof b);
	CHECK(n > 0 && proto_stream_params_decode(b, (size_t)n, &p2) == 0 && memcmp(&s.params, &p2, sizeof p2) == 0);

	struct proto_close c, c2;
	memset(&c, 0, sizeof c);
	c.reason = 1;
	snprintf(c.text, sizeof c.text, "screen locked");
	n = proto_close_encode(&c, b, sizeof b);
	CHECK(n > 0 && proto_close_decode(b, (size_t)n, &c2) == 0 && memcmp(&c, &c2, sizeof c) == 0);

	uint8_t argb[16 * 16 * 4];
	for (size_t i = 0; i < sizeof argb; i++) argb[i] = (uint8_t)i;
	struct proto_cursor_shape cs = { .shape_id = 5, .hot_x = -2, .hot_y = 3, .width = 16, .height = 16, .argb = argb }, cs2;
	n = proto_cursor_shape_encode(&cs, b, sizeof b);
	CHECK(n == 10 + 16 * 16 * 4 && proto_cursor_shape_decode(b, (size_t)n, &cs2) == 0 && cs2.hot_x == -2 &&
	      cs2.hot_y == 3 && memcmp(cs2.argb, argb, sizeof argb) == 0);

	struct proto_feedback f, f2;
	memset(&f, 0, sizeof f);
	f.flags = PROTO_FB_NEED_IDR; f.newest_frame_id = 900; f.held_base = 880; f.held_bitmap = 0xfffffffffffffff5ull;
	f.nack_count = 2; f.loss_permille = 21; f.queue_delay_us = 1500; f.recv_kbps = 18000; f.jitter_us = 800;
	f.decode_ms_x10 = 31;
	f.nacks[0] = (struct proto_nack){ 881, 0, 3 };
	f.nacks[1] = (struct proto_nack){ 883, 1, 1 };
	n = proto_feedback_encode(&f, b, sizeof b);
	CHECK(n > 0 && proto_feedback_decode(b, (size_t)n, &f2) == 0 && memcmp(&f, &f2, sizeof f) == 0);

	struct proto_input in, in2;
	memset(&in, 0, sizeof in);
	in.count = 3;
	for (int i = 0; i < 3; i++)
		in.events[i] = (struct proto_input_event){ .seq = (uint32_t)(10 + i), .kind = PROTO_IN_MOVE, .pointer_id = (uint8_t)i,
							   .tool = PROTO_TOOL_STYLUS, .buttons = 1, .x = (uint16_t)(100 * i),
							   .y = 65535, .pressure = 300, .tilt_x = -45, .tilt_y = 45, .time_us = 123456u };
	n = proto_input_encode(&in, b, sizeof b);
	CHECK(n == 1 + 3 * 20 && proto_input_decode(b, (size_t)n, &in2) == 0 && memcmp(&in, &in2, sizeof in) == 0);

	struct proto_time_resp tr = { 1, 2, 3 }, tr2;
	n = proto_time_resp_encode(&tr, b, sizeof b);
	CHECK(n == 24 && proto_time_resp_decode(b, (size_t)n, &tr2) == 0 && tr2.t2 == 2 && tr2.t3 == 3);

	uint32_t u;
	n = proto_u32_encode(0xfeedfaceu, b, sizeof b);
	CHECK(n == 4 && proto_u32_decode(b, 4, &u) == 0 && u == 0xfeedfaceu);

	struct proto_video_hdr v, v2;
	memset(&v, 0, sizeof v);
	v.frame_id = 70000; v.ref_frame_id = PROTO_NO_REF; v.capture_us = 0x123456789abcdefull; v.send_us = 0xfedcba98u; v.frame_bytes = 250000;
	v.epoch = 2; v.block_index = 1; v.block_count = 2; v.shard_index = 200; v.data_shards = 200; v.parity_shards = 55;
	v.vflags = PROTO_VFLAG_IDR | PROTO_VFLAG_NEW_EPOCH;
	n = proto_video_hdr_write(&v, b, sizeof b);
	CHECK(n == PROTO_VIDEO_HDR_SIZE && proto_video_hdr_read(b, (size_t)n, &v2) == 0 && memcmp(&v, &v2, sizeof v) == 0);
}

/* every strict prefix of a valid message must be rejected, and never read past the end */
#define PREFIXES(name, buf, n, decode_call)                                                         \
	for (int cut_ = 0; cut_ < (n); cut_++) {                                                    \
		int rc_ = (decode_call);                                                            \
		if (rc_ == 0) { fprintf(stderr, "%s: prefix of %d bytes accepted\n", name, cut_); failures++; } \
	}

static void test_truncation(void)
{
	uint8_t b[4096], tight[4096];
	int n;

	struct proto_hello h = sample_hello(), hd;
	n = proto_hello_encode(&h, b, sizeof b);
	PREFIXES("hello", b, n, (memcpy(tight, b, (size_t)cut_), proto_hello_decode(tight, (size_t)cut_, &hd)));

	struct proto_setup s = sample_setup(), sd;
	n = proto_setup_encode(&s, b, sizeof b);
	PREFIXES("setup", b, n, (memcpy(tight, b, (size_t)cut_), proto_setup_decode(tight, (size_t)cut_, &sd)));

	struct proto_feedback f = { .nack_count = 4 }, fd;
	n = proto_feedback_encode(&f, b, sizeof b);
	PREFIXES("feedback", b, n, (memcpy(tight, b, (size_t)cut_), proto_feedback_decode(tight, (size_t)cut_, &fd)));

	struct proto_input in = { .count = 2 }, ind;
	n = proto_input_encode(&in, b, sizeof b);
	PREFIXES("input", b, n, (memcpy(tight, b, (size_t)cut_), proto_input_decode(tight, (size_t)cut_, &ind)));

	struct proto_video_hdr v = { .data_shards = 4, .parity_shards = 1, .block_count = 1 }, vd;
	n = proto_video_hdr_write(&v, b, sizeof b);
	PREFIXES("video", b, n, (memcpy(tight, b, (size_t)cut_), proto_video_hdr_read(tight, (size_t)cut_, &vd)));

	/* an encoder given too small a buffer fails instead of overrunning */
	struct proto_hello h2 = sample_hello();
	for (size_t cap = 0; cap < 60; cap++) CHECK(proto_hello_encode(&h2, tight, cap) < 0);
}

static void test_tlvs_and_limits(void)
{
	uint8_t b[4096];
	struct proto_hello h = sample_hello(), hd;
	int n = proto_hello_encode(&h, b, sizeof b);

	/* unknown TLVs after the fixed fields are skipped */
	static const uint8_t tlv[] = { 0x99, 0x00, 0x03, 0x00, 1, 2, 3, 0x42, 0x00, 0x00, 0x00 };
	memcpy(b + n, tlv, sizeof tlv);
	CHECK(proto_hello_decode(b, (size_t)n + sizeof tlv, &hd) == 0 && hd.native_width == 2304);
	/* a TLV that claims more bytes than remain, and a dangling partial TLV header, are errors */
	b[n + 2] = 0x40;
	CHECK(proto_hello_decode(b, (size_t)n + sizeof tlv, &hd) < 0);
	CHECK(proto_hello_decode(b, (size_t)n + 2, &hd) < 0);

	/* strings: 63 characters fit, 64 do not */
	memset(h.device_name, 'x', 63);
	h.device_name[63] = 0;
	CHECK(proto_hello_encode(&h, b, sizeof b) > 0);
	memset(h.device_name, 'x', 64);
	CHECK(proto_hello_encode(&h, b, sizeof b) == -EINVAL);
	h = sample_hello();
	h.n_refresh = PROTO_MAX_RATES + 1;
	CHECK(proto_hello_encode(&h, b, sizeof b) == -EINVAL);
	h = sample_hello();
	h.orientation = 4;
	CHECK(proto_hello_encode(&h, b, sizeof b) == -EINVAL);

	/* a received list or string over its limit is rejected, not truncated */
	h = sample_hello();
	n = proto_hello_encode(&h, b, sizeof b);
	uint8_t big[4096];
	memcpy(big, b, (size_t)n);
	big[2 + 2 + 2 + (int)strlen(h.device_name) + 4 + 6 + 1 + 8] = 0xff; /* n_refresh low byte: 255 */
	CHECK(proto_hello_decode(big, (size_t)n, &hd) < 0);

	/* parameter sets over the limit */
	struct proto_setup s = sample_setup();
	s.params.param_sets_len = PROTO_MAX_PARAM_SETS + 1;
	CHECK(proto_setup_encode(&s, b, sizeof b) == -EINVAL);

	/* cursor shapes: size limits and length consistency */
	uint8_t argb[4];
	struct proto_cursor_shape cs = { .shape_id = 1, .width = 129, .height = 1, .argb = argb };
	CHECK(proto_cursor_shape_encode(&cs, b, sizeof b) == -EINVAL);
	cs.width = 1;
	n = proto_cursor_shape_encode(&cs, b, sizeof b);
	CHECK(n == 14 && proto_cursor_shape_decode(b, (size_t)n - 1, &cs) < 0);

	/* video header geometry: parity can bring the total to 255 but not past it; blocks must be consistent */
	struct proto_video_hdr v = { .data_shards = 200, .parity_shards = 56, .block_count = 1 };
	CHECK(proto_video_hdr_write(&v, b, sizeof b) == -EINVAL);
	v.parity_shards = 55;
	CHECK(proto_video_hdr_write(&v, b, sizeof b) == PROTO_VIDEO_HDR_SIZE);
	v.block_index = 1;
	CHECK(proto_video_hdr_write(&v, b, sizeof b) == -EINVAL);
	v = (struct proto_video_hdr){ .data_shards = 0, .block_count = 1 };
	CHECK(proto_video_hdr_write(&v, b, sizeof b) == -EINVAL);

	/* input: unknown event kind or tool is rejected */
	struct proto_input in = { .count = 1 }, ind;
	in.events[0].kind = 9;
	n = proto_input_encode(&in, b, sizeof b);
	CHECK(proto_input_decode(b, (size_t)n, &ind) < 0);
	in.count = PROTO_MAX_INPUT_EVENTS + 1;
	CHECK(proto_input_encode(&in, b, sizeof b) == -EINVAL);
}

static void test_ctl_stream(void)
{
	uint8_t a[64], b[64], stream[256];
	uint8_t tok[8];
	proto_token_encode(0x1122334455667788ull, tok, sizeof tok);
	int na = proto_ctl_frame_write(PROTO_CTL_PING, tok, 8, a, sizeof a);
	int nb = proto_ctl_frame_write(PROTO_CTL_READY, NULL, 0, b, sizeof b);
	CHECK(na == 16 && nb == 8);
	memcpy(stream, a, (size_t)na);
	memcpy(stream + na, b, (size_t)nb);

	uint16_t type;
	const uint8_t *payload;
	size_t plen;
	/* a stream is consumed frame by frame; every strict prefix of a frame asks for more data */
	for (int cut = 0; cut < na; cut++) CHECK(proto_ctl_frame_parse(stream, (size_t)cut, &type, &payload, &plen) == 0);
	CHECK(proto_ctl_frame_parse(stream, (size_t)na + nb, &type, &payload, &plen) == na && type == PROTO_CTL_PING && plen == 8);
	uint64_t t;
	CHECK(proto_token_decode(payload, plen, &t) == 0 && t == 0x1122334455667788ull);
	CHECK(proto_ctl_frame_parse(stream + na, (size_t)nb, &type, &payload, &plen) == nb && type == PROTO_CTL_READY && plen == 0);

	/* a length below the 4-byte minimum or over the limit is invalid, not "wait for more" */
	uint8_t bad[8] = { 3, 0, 0, 0, 0, 0, 0, 0 };
	CHECK(proto_ctl_frame_parse(bad, 8, &type, &payload, &plen) < 0);
	bad[0] = 0xff; bad[1] = 0xff; bad[2] = 0xff; bad[3] = 0x7f;
	CHECK(proto_ctl_frame_parse(bad, 8, &type, &payload, &plen) < 0);
	CHECK(proto_ctl_frame_write(PROTO_CTL_PING, tok, PROTO_MAX_CTL_PAYLOAD + 1, a, sizeof a) == -EMSGSIZE);
}

/* ---- mutation fuzz */

static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint32_t rnd(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 7;
	rng ^= rng << 17;
	return (uint32_t)(rng >> 16);
}

static void mutate(uint8_t *buf, size_t *len, size_t cap)
{
	int k = 1 + (int)(rnd() % 4);
	while (k--) {
		switch (rnd() % 4) {
		case 0: if (*len) buf[rnd() % *len] = (uint8_t)rnd(); break;           /* flip a byte */
		case 1: if (*len) buf[rnd() % *len] ^= (uint8_t)(1u << (rnd() % 8)); break; /* flip a bit */
		case 2: if (*len) *len = rnd() % (*len + 1); break;                    /* truncate */
		case 3: if (*len < cap) buf[(*len)++] = (uint8_t)rnd(); break;         /* extend */
		}
	}
}

static void fuzz_one(const uint8_t *seed, size_t seed_len, int (*decode)(const uint8_t *, size_t))
{
	uint8_t *buf = malloc(seed_len + 16);
	for (int i = 0; i < 20000; i++) {
		size_t len = seed_len;
		memcpy(buf, seed, seed_len);
		mutate(buf, &len, seed_len + 8);
		/* an exactly-sized heap block, so ASan flags any read past `len` */
		uint8_t *tight = malloc(len ? len : 1);
		memcpy(tight, buf, len);
		(void)decode(tight, len);
		free(tight);
	}
	for (int i = 0; i < 5000; i++) { /* pure noise */
		size_t len = rnd() % (seed_len + 8);
		uint8_t *tight = malloc(len ? len : 1);
		for (size_t j = 0; j < len; j++) tight[j] = (uint8_t)rnd();
		(void)decode(tight, len);
		free(tight);
	}
	free(buf);
}

static int dec_hello(const uint8_t *b, size_t n)
{
	struct proto_hello m;
	int rc = proto_hello_decode(b, n, &m);
	if (rc == 0 && (m.n_refresh > PROTO_MAX_RATES || m.n_codecs > PROTO_MAX_CODECS || m.orientation > 3 ||
			strnlen(m.device_name, PROTO_NAME_MAX) >= PROTO_NAME_MAX)) { fprintf(stderr, "hello invariant\n"); failures++; }
	return rc;
}
static int dec_setup(const uint8_t *b, size_t n)
{
	struct proto_setup m;
	int rc = proto_setup_decode(b, n, &m);
	if (rc == 0 && m.params.param_sets_len > PROTO_MAX_PARAM_SETS) { fprintf(stderr, "setup invariant\n"); failures++; }
	return rc;
}
static int dec_feedback(const uint8_t *b, size_t n)
{
	struct proto_feedback m;
	int rc = proto_feedback_decode(b, n, &m);
	if (rc == 0 && m.nack_count > PROTO_MAX_NACKS) { fprintf(stderr, "feedback invariant\n"); failures++; }
	return rc;
}
static int dec_input(const uint8_t *b, size_t n)
{
	struct proto_input m;
	int rc = proto_input_decode(b, n, &m);
	if (rc == 0 && m.count > PROTO_MAX_INPUT_EVENTS) { fprintf(stderr, "input invariant\n"); failures++; }
	return rc;
}
static int dec_video(const uint8_t *b, size_t n)
{
	struct proto_video_hdr m;
	int rc = proto_video_hdr_read(b, n, &m);
	if (rc == 0 && (m.data_shards == 0 || m.block_index >= m.block_count || m.data_shards + m.parity_shards > 255)) {
		fprintf(stderr, "video invariant\n");
		failures++;
	}
	return rc;
}
static int dec_cursor(const uint8_t *b, size_t n)
{
	struct proto_cursor_shape m;
	int rc = proto_cursor_shape_decode(b, n, &m);
	if (rc == 0 && (m.width > PROTO_MAX_CURSOR_DIM || m.argb < b || m.argb + (size_t)m.width * m.height * 4 > b + n)) {
		fprintf(stderr, "cursor invariant\n");
		failures++;
	}
	return rc;
}
static int dec_frame(const uint8_t *b, size_t n)
{
	uint16_t t;
	const uint8_t *p;
	size_t pl;
	int rc = proto_ctl_frame_parse(b, n, &t, &p, &pl);
	if (rc > 0 && ((size_t)rc > n || p < b || p + pl > b + n)) { fprintf(stderr, "frame invariant\n"); failures++; }
	return rc;
}

static void test_fuzz(void)
{
	uint8_t b[4096];
	int n;
	struct proto_hello h = sample_hello();
	n = proto_hello_encode(&h, b, sizeof b);
	fuzz_one(b, (size_t)n, dec_hello);

	struct proto_setup s = sample_setup();
	n = proto_setup_encode(&s, b, sizeof b);
	fuzz_one(b, (size_t)n, dec_setup);

	struct proto_feedback f = { .nack_count = 3, .newest_frame_id = 5 };
	n = proto_feedback_encode(&f, b, sizeof b);
	fuzz_one(b, (size_t)n, dec_feedback);

	struct proto_input in = { .count = 5 };
	n = proto_input_encode(&in, b, sizeof b);
	fuzz_one(b, (size_t)n, dec_input);

	struct proto_video_hdr v = { .data_shards = 10, .parity_shards = 2, .block_count = 1 };
	n = proto_video_hdr_write(&v, b, sizeof b);
	fuzz_one(b, (size_t)n, dec_video);

	uint8_t argb[8 * 8 * 4] = { 0 };
	struct proto_cursor_shape cs = { .shape_id = 1, .width = 8, .height = 8, .argb = argb };
	n = proto_cursor_shape_encode(&cs, b, sizeof b);
	fuzz_one(b, (size_t)n, dec_cursor);

	uint8_t tok[8];
	proto_token_encode(1, tok, 8);
	n = proto_ctl_frame_write(PROTO_CTL_PING, tok, 8, b, sizeof b);
	fuzz_one(b, (size_t)n, dec_frame);
}

int main(void)
{
	test_vectors();
	test_roundtrips();
	test_truncation();
	test_tlvs_and_limits();
	test_ctl_stream();
	test_fuzz();
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	puts("proto: all checks passed");
	return 0;
}
