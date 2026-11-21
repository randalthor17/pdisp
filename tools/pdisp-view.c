/* pdisp-view: a desktop test client that plays the phone's role. It connects to pdispd, receives the HEVC stream over UDP
 * with the same loss handling a phone uses (vrecv), decodes it with libavcodec, and reports statistics. It can also check
 * the decoded picture against the test pattern (tools/pattern.h) and save a snapshot.
 *
 *   pdisp-view HOST PORT [-W w] [-H h] [-r hz[,hz...]] [-D dpi] [-S secs] [-o snap.png] [-c] [-v] [-L loss%]
 *
 * Exit: 0 success (and, with -c, the pattern matched), 1 failure, 3 rejected by the daemon.
 */
#include <errno.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "net.h"
#include "pattern.h"
#include "png.h"
#include "proto.h"
#include "vrecv.h"

static volatile sig_atomic_t stop;
static void on_signal(int sig) { (void)sig; stop = 1; }

static uint64_t t_start;
#define LOG(...) do { fprintf(stderr, "[%8.3f] ", (net_now_us() - t_start) / 1e6); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)

struct viewer {
	AVCodecContext *dec;
	AVPacket *pkt;
	AVFrame *frame;
	struct SwsContext *sws;
	uint8_t *bgra;           /* the newest decoded picture, converted */
	int width, height;
	bool verbose;

	uint64_t decoded, errors;
	uint64_t *lat_us;        /* capture to decode-complete latency, per frame */
	size_t n_lat, cap_lat;
	uint64_t first_frame_us, last_frame_us;
	double decode_ms_sum;
	uint64_t drop_rng;       /* -L: drop this share of incoming datagrams (a crude loss injector) */
	double drop_rate;

	/* decoding runs on its own thread, like a phone's async MediaCodec: the network loop must never wait for it */
	pthread_t thread;
	pthread_mutex_t mu;
	pthread_cond_t cv;
	struct job *jobs;
	unsigned job_head, job_count;
	bool quit;
	atomic_bool decoder_failed;
	AVFrame *last;           /* newest decoded picture, converted at exit */
};

#define JOB_RING 64
struct job {
	uint32_t id, ref;
	uint64_t capture_us;
	size_t size;
	uint8_t *data;
};

static struct viewer V;

/* decode one access unit; runs on the decode thread */
static void decode_job(const struct job *jb)
{
	uint64_t t0 = net_now_us();
	/* libavcodec reads past the end of its input, so the access unit is copied into a padded buffer */
	if (av_new_packet(V.pkt, (int)jb->size) < 0) { atomic_store(&V.decoder_failed, true); return; }
	memcpy(V.pkt->data, jb->data, jb->size);
	int rc = avcodec_send_packet(V.dec, V.pkt);
	av_packet_unref(V.pkt);
	if (rc < 0) { V.errors++; atomic_store(&V.decoder_failed, true); return; }
	for (;;) {
		rc = avcodec_receive_frame(V.dec, V.frame);
		if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) break;
		if (rc < 0) { V.errors++; atomic_store(&V.decoder_failed, true); return; }
		uint64_t now = net_now_us();
		V.decoded++;
		V.decode_ms_sum += (now - t0) / 1000.0;
		if (!V.first_frame_us) V.first_frame_us = now;
		V.last_frame_us = now;
		if (V.n_lat == V.cap_lat) {
			V.cap_lat = V.cap_lat ? V.cap_lat * 2 : 1024;
			V.lat_us = realloc(V.lat_us, V.cap_lat * sizeof *V.lat_us);
		}
		V.lat_us[V.n_lat++] = now > jb->capture_us ? now - jb->capture_us : 0;
		V.width = V.frame->width;
		V.height = V.frame->height;
		if (!V.last) V.last = av_frame_alloc();
		av_frame_unref(V.last);
		av_frame_ref(V.last, V.frame);
		if (V.verbose) LOG("decoded frame %u (ref %d) %zu bytes, latency %.1f ms", jb->id,
				   jb->ref == PROTO_NO_REF ? -1 : (int)jb->ref, jb->size, V.lat_us[V.n_lat - 1] / 1000.0);
	}
}

static void *decode_thread(void *arg)
{
	(void)arg;
	for (;;) {
		pthread_mutex_lock(&V.mu);
		while (!V.job_count && !V.quit) pthread_cond_wait(&V.cv, &V.mu);
		if (!V.job_count) { pthread_mutex_unlock(&V.mu); return NULL; }
		struct job jb = V.jobs[V.job_head];
		V.job_head = (V.job_head + 1) % JOB_RING;
		V.job_count--;
		pthread_mutex_unlock(&V.mu);
		decode_job(&jb);
		free(jb.data);
	}
}

/* the receiver's decode callback: hand the frame to the decode thread (the data is only valid during this call) */
static int decode_cb(const struct vrecv_frame *f, void *user)
{
	(void)user;
	if (atomic_load(&V.decoder_failed)) return -1;
	uint8_t *copy = malloc(f->size);
	if (!copy) return -1;
	memcpy(copy, f->data, f->size);
	pthread_mutex_lock(&V.mu);
	if (V.job_count == JOB_RING) { /* the decoder cannot keep up: refuse, which makes the receiver ask for an IDR */
		pthread_mutex_unlock(&V.mu);
		free(copy);
		return -1;
	}
	V.jobs[(V.job_head + V.job_count++) % JOB_RING] = (struct job){ f->frame_id, f->ref_frame_id, f->capture_us, f->size, copy };
	pthread_cond_signal(&V.cv);
	pthread_mutex_unlock(&V.mu);
	return 0;
}

static int cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
	return x < y ? -1 : x > y;
}

/* Fraction of sampled pixels (centres of the pattern's 32 px squares, away from the lossy edges) within tolerance. */
static double pattern_match(const uint8_t *bgra, int w, int h)
{
	long total = 0, good = 0;
	for (int y = 8; y < h - 8; y += 32)
		for (int x = 8; x < w - 8; x += 32) {
			uint32_t want = pattern_pixel((unsigned)(x + 8), (unsigned)(y + 8), (unsigned)w, (unsigned)h);
			const uint8_t *p = bgra + ((size_t)(y + 8) * w + (x + 8)) * 4;
			int db = abs((int)p[0] - (int)(want & 0xff)), dg = abs((int)p[1] - (int)((want >> 8) & 0xff)),
			    dr = abs((int)p[2] - (int)((want >> 16) & 0xff));
			total++;
			if (db <= 24 && dg <= 24 && dr <= 24) good++;
		}
	return total ? (double)good / total : 0;
}

static void usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s HOST PORT [options]\n"
		"  -W w -H h     the \"native\" size reported in HELLO (default 2304x1440)\n"
		"  -r hz[,hz]    refresh rates reported (default 60)\n"
		"  -D dpi        density reported (default 262)\n"
		"  -S secs       run this long after the stream starts (default 10)\n"
		"  -o file.png   save the last decoded picture\n"
		"  -c            require the decoded picture to match the test pattern\n"
		"  -L pct        drop this percentage of incoming datagrams at random\n"
		"  -v            log every decoded frame\n",
		argv0);
}

int main(int argc, char **argv)
{
	if (argc < 3) { usage(argv[0]); return 2; }
	const char *host = argv[1];
	unsigned port = (unsigned)atoi(argv[2]);
	unsigned nw = 2304, nh = 1440, dpi = 262;
	double secs = 10, drop_pct = 0;
	const char *snap = NULL, *rates = "60";
	bool check = false;
	optind = 3;
	int opt;
	while ((opt = getopt(argc, argv, "W:H:r:D:S:o:cL:vh")) != -1) {
		switch (opt) {
		case 'W': nw = (unsigned)atoi(optarg); break;
		case 'H': nh = (unsigned)atoi(optarg); break;
		case 'r': rates = optarg; break;
		case 'D': dpi = (unsigned)atoi(optarg); break;
		case 'S': secs = atof(optarg); break;
		case 'o': snap = optarg; break;
		case 'c': check = true; break;
		case 'L': drop_pct = atof(optarg); break;
		case 'v': V.verbose = true; break;
		default: usage(argv[0]); return 2;
		}
	}
	struct sigaction sa = { .sa_handler = on_signal };
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	t_start = net_now_us();
	V.drop_rate = drop_pct / 100.0;
	V.drop_rng = 0x2545f4914f6cdd1dull;

	/* ---- control channel: HELLO, SETUP */
	int cfd = net_tcp_connect(host, port);
	if (cfd < 0) { fprintf(stderr, "cannot connect to %s:%u: %s\n", host, port, strerror(-cfd)); return 1; }
	struct net_ctl ctl;
	net_ctl_init(&ctl, cfd);

	struct proto_hello hello;
	memset(&hello, 0, sizeof hello);
	hello.proto_min = hello.proto_max = PROTO_VERSION;
	snprintf(hello.device_name, sizeof hello.device_name, "pdisp-view");
	hello.device_caps = PROTO_CAP_TOUCH;
	hello.native_width = (uint16_t)nw; hello.native_height = (uint16_t)nh; hello.density_dpi = (uint16_t)dpi;
	for (char *s = (char *)rates, *e; *s && hello.n_refresh < PROTO_MAX_RATES; s = *e ? e + 1 : e) {
		hello.refresh_mhz[hello.n_refresh++] = (uint32_t)(strtod(s, &e) * 1000 + 0.5);
		if (e == s) break;
	}
	hello.n_codecs = 1; hello.codecs[0] = PROTO_CODEC_HEVC;
	hello.max_touch_points = 10; hello.max_udp_payload = 1472;
	snprintf(hello.app_version, sizeof hello.app_version, "test");
	uint8_t b[2048];
	int n = proto_hello_encode(&hello, b, sizeof b);
	if (n < 0 || net_ctl_send(&ctl, PROTO_CTL_HELLO, b, (size_t)n) != 0) { fprintf(stderr, "cannot send HELLO\n"); return 1; }

	uint16_t type;
	const uint8_t *payload;
	size_t plen;
	int r = net_ctl_recv(&ctl, &type, &payload, &plen, 20000);
	if (r != 1) { fprintf(stderr, "no answer to HELLO (%d)\n", r); return 1; }
	if (type == PROTO_CTL_REJECT) {
		struct proto_reject rj;
		if (proto_reject_decode(payload, plen, &rj) == 0) fprintf(stderr, "rejected: %s\n", rj.text);
		return 3;
	}
	struct proto_setup su;
	if (type != PROTO_CTL_SETUP || proto_setup_decode(payload, plen, &su) != 0) { fprintf(stderr, "bad SETUP\n"); return 1; }
	LOG("SETUP: %ux%u@%.3f, session %08x, %u kbit/s, %u bytes of parameter sets", su.params.width, su.params.height,
	    su.params.refresh_mhz / 1000.0, su.session_id, su.params.bitrate_kbps, su.params.param_sets_len);

	/* ---- decoder */
	const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_HEVC);
	V.dec = avcodec_alloc_context3(codec);
	V.dec->thread_count = 0;
	V.dec->flags |= AV_CODEC_FLAG_LOW_DELAY;
	V.dec->extradata = av_mallocz(su.params.param_sets_len + AV_INPUT_BUFFER_PADDING_SIZE);
	memcpy(V.dec->extradata, su.params.param_sets, su.params.param_sets_len);
	V.dec->extradata_size = su.params.param_sets_len;
	if (avcodec_open2(V.dec, codec, NULL) < 0) { fprintf(stderr, "cannot open the HEVC decoder\n"); return 1; }
	V.pkt = av_packet_alloc();
	V.frame = av_frame_alloc();
	pthread_mutex_init(&V.mu, NULL);
	pthread_cond_init(&V.cv, NULL);
	V.jobs = calloc(JOB_RING, sizeof *V.jobs);
	pthread_create(&V.thread, NULL, decode_thread, NULL);

	struct vrecv_config rc = { .session_id = su.session_id, .shard_size = su.params.shard_size };
	struct vrecv_callbacks rcb = { .decode = decode_cb };
	struct vrecv *vr = vrecv_create(&rc, &rcb);

	/* ---- UDP: tell the daemon where to send, then READY */
	int ufd = net_udp_bind(0);
	struct sockaddr_in to;
	{
		struct sockaddr_in peer;
		socklen_t pl = sizeof peer;
		getpeername(cfd, (struct sockaddr *)&peer, &pl);
		to = peer;
		to.sin_port = htons(su.udp_port);
	}
	uint64_t udp_seq = 0;
	struct proto_pkt_hdr hh = { .session_id = su.session_id, .type = PROTO_PKT_HELLO_UDP };
	uint8_t hb[PROTO_PKT_HDR_SIZE];
	hh.seq = udp_seq++;
	proto_pkt_hdr_write(&hh, hb, sizeof hb);
	sendto(ufd, hb, sizeof hb, 0, (struct sockaddr *)&to, sizeof to);
	net_ctl_send(&ctl, PROTO_CTL_READY, NULL, 0);
	LOG("sent READY");

	/* ---- receive */
	uint64_t next_fb = net_now_us() + 10000, next_hello = net_now_us() + 100000;
	uint64_t stream_start = 0, end_us = 0;
	uint8_t pkt[2048];
	while (!stop) {
		uint64_t now = net_now_us();
		if (end_us && now >= end_us) break;
		struct pollfd p[2] = { { .fd = ufd, .events = POLLIN }, { .fd = cfd, .events = POLLIN } };
		poll(p, 2, 2);
		now = net_now_us();
		for (;;) {
			ssize_t got = recv(ufd, pkt, sizeof pkt, 0);
			if (got < 0) break;
			if (V.drop_rate > 0) {
				V.drop_rng ^= V.drop_rng << 13; V.drop_rng ^= V.drop_rng >> 7; V.drop_rng ^= V.drop_rng << 17;
				if ((double)(V.drop_rng >> 16 & 0xfffff) / 1048576.0 < V.drop_rate) continue;
			}
			if (!stream_start) {
				stream_start = now;
				end_us = now + (uint64_t)(secs * 1e6);
				LOG("first packet from the daemon");
			}
			vrecv_on_packet(vr, pkt, (size_t)got, now);
		}
		if (p[1].revents & (POLLIN | POLLHUP)) {
			r = net_ctl_recv(&ctl, &type, &payload, &plen, 0);
			if (r == 1 && type == PROTO_CTL_CLOSE) { LOG("the daemon closed the session"); break; }
			if (r < 0) { LOG("control connection closed"); break; }
		}
		if (atomic_exchange(&V.decoder_failed, false)) vrecv_decoder_error(vr);
		vrecv_tick(vr, now);
		if (!stream_start && now >= next_hello) { /* until the stream starts, keep telling the daemon where we are */
			next_hello = now + 100000;
			hh.seq = udp_seq++;
			proto_pkt_hdr_write(&hh, hb, sizeof hb);
			sendto(ufd, hb, sizeof hb, 0, (struct sockaddr *)&to, sizeof to);
		}
		if (now >= next_fb) {
			next_fb = now + 10000;
			int fn = vrecv_feedback_packet(vr, now, pkt, sizeof pkt);
			if (fn > 0) sendto(ufd, pkt, (size_t)fn, 0, (struct sockaddr *)&to, sizeof to);
		}
	}

	/* ---- report: stop the decoder thread first, then convert the newest picture for the checks */
	pthread_mutex_lock(&V.mu);
	V.quit = true;
	pthread_cond_signal(&V.cv);
	pthread_mutex_unlock(&V.mu);
	pthread_join(V.thread, NULL);
	if (V.last) {
		V.sws = sws_getCachedContext(V.sws, V.last->width, V.last->height, (enum AVPixelFormat)V.last->format, V.last->width,
					     V.last->height, AV_PIX_FMT_BGRA, SWS_BILINEAR, NULL, NULL, NULL);
		if (V.sws) {
			/* the stream is tagged BT.709, limited range */
			sws_setColorspaceDetails(V.sws, sws_getCoefficients(SWS_CS_ITU709), 0, sws_getCoefficients(SWS_CS_DEFAULT), 1, 0,
						 1 << 16, 1 << 16);
			V.bgra = malloc((size_t)V.last->width * V.last->height * 4);
			uint8_t *dst[4] = { V.bgra, NULL, NULL, NULL };
			int dst_stride[4] = { V.last->width * 4, 0, 0, 0 };
			sws_scale(V.sws, (const uint8_t *const *)V.last->data, V.last->linesize, 0, V.last->height, dst, dst_stride);
		}
	}
	qsort(V.lat_us, V.n_lat, sizeof *V.lat_us, cmp_u64);
	double span = V.last_frame_us > V.first_frame_us ? (V.last_frame_us - V.first_frame_us) / 1e6 : 1;
	const struct vrecv_stats *vs = vrecv_stats(vr);
	double match = V.bgra ? pattern_match(V.bgra, V.width, V.height) : 0;
	printf("VIEW packets=%llu ", (unsigned long long)vs->packets);
	printf("decoded=%llu fps=%.1f decode_ms_avg=%.2f latency_ms_p50=%.1f p99=%.1f max=%.1f given_up=%llu idr=%llu "
	       "decoder_errors=%llu match=%.1f%%\n",
	       (unsigned long long)V.decoded, V.decoded / span, V.decoded ? V.decode_ms_sum / V.decoded : 0.0,
	       V.n_lat ? V.lat_us[V.n_lat / 2] / 1000.0 : 0.0, V.n_lat ? V.lat_us[(size_t)((V.n_lat - 1) * 0.99)] / 1000.0 : 0.0,
	       V.n_lat ? V.lat_us[V.n_lat - 1] / 1000.0 : 0.0, (unsigned long long)vs->frames_given_up,
	       (unsigned long long)vs->idr_decoded, (unsigned long long)vs->decoder_errors, match * 100);
	if (snap && V.bgra && png_write_bgra(snap, V.bgra, V.width, V.height, V.width * 4) == 0) LOG("wrote %s", snap);

	uint8_t cb[64];
	struct proto_close cl;
	memset(&cl, 0, sizeof cl);
	int cn = proto_close_encode(&cl, cb, sizeof cb);
	if (cn > 0) net_ctl_send(&ctl, PROTO_CTL_CLOSE, cb, (size_t)cn);
	int status = 0;
	if (V.decoded == 0) { fprintf(stderr, "FAIL: nothing was decoded\n"); status = 1; }
	else if (check && match < 0.95) { fprintf(stderr, "FAIL: the decoded picture does not match the pattern (%.1f%%)\n", match * 100); status = 1; }
	vrecv_destroy(vr);
	net_ctl_close(&ctl);
	close(ufd);
	return status;
}
