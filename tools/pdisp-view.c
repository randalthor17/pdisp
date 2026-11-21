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
#include <SDL2/SDL.h>
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

#include "handshake.h"
#include "identity.h"
#include "inputseq.h"
#include "net.h"
#include "pattern.h"
#include "png.h"
#include "proto.h"
#include "stamp.h"
#include "timesync.h"
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
	pthread_mutex_t last_mu; /* guards `last` and `frame_serial` while a window is shown */
	unsigned frame_serial;

	/* -w: a window showing the stream */
	bool window;
	SDL_Window *win;
	SDL_Renderer *ren;
	SDL_Texture *tex;
	int tex_w, tex_h;
	unsigned shown_serial;

	/* clocks: the viewer's own clock may be skewed (-K) to prove the sync works; server_time = client_time + offset */
	int64_t skew_us;
	atomic_llong offset_us;
	atomic_bool have_offset;
	struct timesync ts;
	int64_t sync_rtt_us;
	uint64_t time_resp, pings, pongs, reconfigs, auth_drops;

	/* glass-to-glass: the stamp the application drew into the frame, read back from the decoded picture */
	bool read_stamps;
	uint64_t *glass_us;
	size_t n_glass, cap_glass;

	/* freeze and corruption tracking over the whole run */
	uint64_t max_gap_us;     /* longest time between two decoded frames, after the first 30 */
	struct SwsContext *chk_sws;
	uint8_t *chk_bgra;
	unsigned chk_samples, chk_bad, chk_other; /* bad: partly wrong picture; other: not the pattern at all (compositor showed something else) */
	/* -j: a scripted touch session, played as the phone would send it */
	struct input_tx tx;
	struct sched_ev { uint64_t at_us; struct proto_input_event ev; } *sched;
	size_t n_sched, i_sched;
	uint64_t touch_started_us;
	uint64_t touch_pushed, touch_acked_to;
	const char *snap_path;   /* -o: where a snapshot goes; bad checks save pictures next to it */
	double chk_min;          /* lowest pattern match over the periodic checks */
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
static double pattern_match(const uint8_t *bgra, int w, int h);

/* periodic check of a decoded picture against the test pattern (the animated bar covers at most 3%) */
static void check_picture(const AVFrame *fr)
{
	V.chk_sws = sws_getCachedContext(V.chk_sws, fr->width, fr->height, (enum AVPixelFormat)fr->format, fr->width, fr->height,
					 AV_PIX_FMT_BGRA, SWS_FAST_BILINEAR, NULL, NULL, NULL);
	if (!V.chk_sws) return;
	sws_setColorspaceDetails(V.chk_sws, sws_getCoefficients(SWS_CS_ITU709), 0, sws_getCoefficients(SWS_CS_DEFAULT), 1, 0, 1 << 16, 1 << 16);
	if (!V.chk_bgra) V.chk_bgra = malloc((size_t)fr->width * fr->height * 4);
	uint8_t *dst[4] = { V.chk_bgra, NULL, NULL, NULL };
	int dst_stride[4] = { fr->width * 4, 0, 0, 0 };
	sws_scale(V.chk_sws, (const uint8_t *const *)fr->data, fr->linesize, 0, fr->height, dst, dst_stride);
	double m = pattern_match(V.chk_bgra, fr->width, fr->height);
	if (!V.chk_samples) V.chk_min = 1.0;
	if (m >= 0.05 && m < V.chk_min) V.chk_min = m; /* a different picture altogether says nothing about decoding */
	if (m < 0.9) {
		/* A decode error damages part of a picture. A picture that matches almost nowhere is a different picture: the desktop was
		 * showing something else (niri's overview after a stray hot-corner touch, a notification, a lock screen), decoded faithfully. */
		if (m < 0.05) V.chk_other++; else V.chk_bad++;
		if (V.snap_path && V.chk_bad + V.chk_other <= 3) { /* keep the evidence: what did a mismatching picture look like, and when? */
			char name[512];
			snprintf(name, sizeof name, "%s.bad%u.png", V.snap_path, V.chk_bad + V.chk_other);
			png_write_bgra(name, V.chk_bgra, fr->width, fr->height, fr->width * 4);
			LOG("check %u failed (%.1f%% match) on decoded frame %llu, saved %s", V.chk_samples + 1, m * 100, (unsigned long long)V.decoded, name);
		}
	}
	V.chk_samples++;
}

/* datagrams to the PC: sealed after the handshake */
static int ufd_global;
static struct sec_udp *udp_ctx; /* NULL in insecure mode */
static struct sockaddr_in udp_to;
static void udp_out(const uint8_t *buf, size_t n)
{
	uint8_t tmp[2048 + SEC_UDP_OVERHEAD];
	if (udp_ctx) {
		if (n > 2048) return;
		memcpy(tmp, buf, n);
		int sn = sec_udp_seal(udp_ctx, tmp, n, sizeof tmp);
		if (sn < 0) return;
		buf = tmp;
		n = (size_t)sn;
	}
	sendto(ufd_global, buf, n, 0, (struct sockaddr *)&udp_to, sizeof udp_to);
}

/* the viewer's clock, as the (possibly skewed) clock of the phone it plays */
static uint64_t cnow(void) { return net_now_us() + (uint64_t)V.skew_us; }

/* the PC's clock now, from the viewer's clock and the sync estimate (0 offset until the first exchange completes) */
static uint64_t server_now(void) { return cnow() + (uint64_t)(int64_t)atomic_load(&V.offset_us); }

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
		if (V.decoded > 30 && V.last_frame_us && now - V.last_frame_us > V.max_gap_us) V.max_gap_us = now - V.last_frame_us;
		if (V.decoded > 60 && V.decoded % 20 == 0) check_picture(V.frame);
		V.decode_ms_sum += (now - t0) / 1000.0;
		if (!V.first_frame_us) V.first_frame_us = now;
		V.last_frame_us = now;
		if (V.n_lat == V.cap_lat) {
			V.cap_lat = V.cap_lat ? V.cap_lat * 2 : 1024;
			V.lat_us = realloc(V.lat_us, V.cap_lat * sizeof *V.lat_us);
		}
		uint64_t sn = server_now();
		V.lat_us[V.n_lat++] = sn > jb->capture_us ? sn - jb->capture_us : 0;
		V.width = V.frame->width;
		V.height = V.frame->height;
		if (V.read_stamps && V.frame->format == AV_PIX_FMT_YUV420P) {
			uint64_t stamp;
			if (stamp_read(V.frame->data[0], V.frame->linesize[0], V.frame->width, &stamp)) {
				uint64_t sn2 = server_now() & ((1ull << STAMP_VALUE_BITS) - 1);
				uint64_t d = (sn2 - stamp) & ((1ull << STAMP_VALUE_BITS) - 1); /* the stamp holds the low 48 bits of the clock */
				if (d < 5000000) { /* anything older than 5 s is a misread, not a measurement */
					if (V.n_glass == V.cap_glass) {
						V.cap_glass = V.cap_glass ? V.cap_glass * 2 : 1024;
						V.glass_us = realloc(V.glass_us, V.cap_glass * sizeof *V.glass_us);
					}
					V.glass_us[V.n_glass++] = d;
				}
			}
		}
		pthread_mutex_lock(&V.last_mu);
		if (!V.last) V.last = av_frame_alloc();
		av_frame_unref(V.last);
		av_frame_ref(V.last, V.frame);
		V.frame_serial++;
		pthread_mutex_unlock(&V.last_mu);
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

/* Shows the newest decoded picture in the window, creating or resizing it as needed. Runs on the main thread. */
static void show_picture(void)
{
	pthread_mutex_lock(&V.last_mu);
	if (!V.last || V.shown_serial == V.frame_serial) { pthread_mutex_unlock(&V.last_mu); return; }
	V.shown_serial = V.frame_serial;
	if (V.last->format != AV_PIX_FMT_YUV420P) { pthread_mutex_unlock(&V.last_mu); return; }
	int w = V.last->width, h = V.last->height;
	if (!V.win) {
		int ww = w, wh = h;
		while (ww > 1280 || wh > 800) { ww /= 2; wh /= 2; } /* a window that fits on a normal screen */
		V.win = SDL_CreateWindow("pdisp-view", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, ww, wh, SDL_WINDOW_RESIZABLE);
		V.ren = V.win ? SDL_CreateRenderer(V.win, -1, SDL_RENDERER_ACCELERATED) : NULL;
		if (!V.ren) { fprintf(stderr, "cannot create a window: %s\n", SDL_GetError()); V.window = false; pthread_mutex_unlock(&V.last_mu); return; }
		SDL_SetYUVConversionMode(SDL_YUV_CONVERSION_BT709); /* the stream is tagged BT.709 */
	}
	if (!V.tex || V.tex_w != w || V.tex_h != h) { /* first picture, or the size changed (RECONFIGURE) */
		if (V.tex) SDL_DestroyTexture(V.tex);
		V.tex = SDL_CreateTexture(V.ren, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, w, h);
		V.tex_w = w;
		V.tex_h = h;
		SDL_RenderSetLogicalSize(V.ren, w, h);
	}
	if (V.tex) {
		SDL_UpdateYUVTexture(V.tex, NULL, V.last->data[0], V.last->linesize[0], V.last->data[1], V.last->linesize[1],
				     V.last->data[2], V.last->linesize[2]);
		pthread_mutex_unlock(&V.last_mu);
		SDL_RenderClear(V.ren);
		SDL_RenderCopy(V.ren, V.tex, NULL, NULL);
		SDL_RenderPresent(V.ren);
		return;
	}
	pthread_mutex_unlock(&V.last_mu);
}

/* (Re)opens the HEVC decoder with the parameter sets of the current epoch and starts its thread. */
static int start_decoder(const uint8_t *param_sets, size_t len)
{
	const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_HEVC);
	V.dec = avcodec_alloc_context3(codec);
	V.dec->thread_count = 0;
	V.dec->flags |= AV_CODEC_FLAG_LOW_DELAY;
	V.dec->extradata = av_mallocz(len + AV_INPUT_BUFFER_PADDING_SIZE);
	memcpy(V.dec->extradata, param_sets, len);
	V.dec->extradata_size = (int)len;
	if (avcodec_open2(V.dec, codec, NULL) < 0) return -1;
	if (!V.pkt) V.pkt = av_packet_alloc();
	if (!V.frame) V.frame = av_frame_alloc();
	if (!V.jobs) {
		pthread_mutex_init(&V.last_mu, NULL);
		pthread_mutex_init(&V.mu, NULL);
		pthread_cond_init(&V.cv, NULL);
		V.jobs = calloc(JOB_RING, sizeof *V.jobs);
	}
	V.quit = false;
	atomic_store(&V.decoder_failed, false);
	return pthread_create(&V.thread, NULL, decode_thread, NULL);
}

/* Lets the decode thread finish what is queued, then stops it and frees the decoder. */
static void stop_decoder(void)
{
	pthread_mutex_lock(&V.mu);
	V.quit = true;
	pthread_cond_signal(&V.cv);
	pthread_mutex_unlock(&V.mu);
	pthread_join(V.thread, NULL);
	avcodec_free_context(&V.dec);
}

/* "tap:X,Y swipe:X1,Y1:X2,Y2 pinch:CX,CY:R1:R2 hold:MS": fractions of the screen, as in tools/pdisp-touch-demo.c */
static void sched_add_tool(uint64_t at, uint8_t kind, uint8_t id, double x, double y, uint8_t tool, double pressure, int8_t tilt, uint8_t buttons);
static void sched_add(uint64_t at, uint8_t kind, uint8_t id, double x, double y)
{
	sched_add_tool(at, kind, id, x, y, PROTO_TOOL_FINGER, 0.46, 0, 0);
}

static void sched_add_tool(uint64_t at, uint8_t kind, uint8_t id, double x, double y, uint8_t tool, double pressure, int8_t tilt, uint8_t buttons)
{
	static size_t cap;
	if (V.n_sched == cap) { cap = cap ? cap * 2 : 256; V.sched = realloc(V.sched, cap * sizeof *V.sched); }
	struct proto_input_event e;
	memset(&e, 0, sizeof e);
	e.kind = kind; e.pointer_id = id; e.tool = tool; e.buttons = buttons; e.tilt_x = tilt; e.tilt_y = (int8_t)-tilt;
	e.pressure = (uint16_t)(pressure < 0 ? 0 : pressure > 1 ? 65535 : pressure * 65535 + 0.5);
	e.x = (uint16_t)(x < 0 ? 0 : x > 1 ? 65535 : x * 65535 + 0.5);
	e.y = (uint16_t)(y < 0 ? 0 : y > 1 ? 65535 : y * 65535 + 0.5);
	V.sched[V.n_sched++] = (struct sched_ev){ at, e };
}

static int parse_touch_script(const char *script)
{
	char *copy = strdup(script), *save = NULL;
	uint64_t t = 0;
	for (char *step = strtok_r(copy, " ", &save); step; step = strtok_r(NULL, " ", &save)) {
		double a, b, c, d;
		int ms;
		if (sscanf(step, "tap:%lf,%lf", &a, &b) == 2) {
			sched_add(t, PROTO_IN_DOWN, 0, a, b); t += 60000; sched_add(t, PROTO_IN_UP, 0, a, b);
		} else if (sscanf(step, "swipe:%lf,%lf:%lf,%lf", &a, &b, &c, &d) == 4) {
			sched_add(t, PROTO_IN_DOWN, 0, a, b);
			for (int i = 1; i <= 20; i++) { t += 8000; sched_add(t, PROTO_IN_MOVE, 0, a + (c - a) * i / 20, b + (d - b) * i / 20); }
			sched_add(t, PROTO_IN_UP, 0, c, d);
		} else if (sscanf(step, "pinch:%lf,%lf:%lf:%lf", &a, &b, &c, &d) == 4) {
			sched_add(t, PROTO_IN_DOWN, 0, a - c, b); sched_add(t, PROTO_IN_DOWN, 1, a + c, b);
			for (int i = 1; i <= 20; i++) {
				double rr = c + (d - c) * i / 20;
				t += 8000; sched_add(t, PROTO_IN_MOVE, 0, a - rr, b); sched_add(t, PROTO_IN_MOVE, 1, a + rr, b);
			}
			sched_add(t, PROTO_IN_UP, 0, a - d, b); sched_add(t, PROTO_IN_UP, 1, a + d, b);
		} else if (sscanf(step, "pen:%lf,%lf:%lf,%lf", &a, &b, &c, &d) == 4 || sscanf(step, "erase:%lf,%lf:%lf,%lf", &a, &b, &c, &d) == 4) {
			/* hover, press, a stroke with rising pressure and the barrel button held for its second half, lift, leave */
			uint8_t tool = step[0] == 'e' ? PROTO_TOOL_ERASER : PROTO_TOOL_STYLUS;
			sched_add_tool(t, PROTO_IN_HOVER, 0, a, b, tool, 0.5, 20, 0); t += 40000;
			sched_add_tool(t, PROTO_IN_DOWN, 0, a, b, tool, 0.1, 20, 0);
			for (int i = 1; i <= 20; i++) { t += 8000; sched_add_tool(t, PROTO_IN_MOVE, 0, a + (c - a) * i / 20, b + (d - b) * i / 20, tool, 0.1 + 0.9 * i / 20, (int8_t)(20 + i), i > 10 ? 1 : 0); }
			sched_add_tool(t, PROTO_IN_UP, 0, c, d, tool, 0, 40, 0); t += 40000;
			sched_add_tool(t, PROTO_IN_CANCEL, 0, c, d, tool, 0, 0, 0);
		} else if (sscanf(step, "hold:%d", &ms) == 1) {
			t += (uint64_t)ms * 1000;
			continue;
		} else { free(copy); return -1; }
		t += 150000;
	}
	free(copy);
	return 0;
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
		"  -w            show the stream in a window (SDL2)\n"
		"  -G            read the timestamp the pattern client draws (-T there) and report glass-to-glass latency\n"
		"  -L pct        drop this percentage of incoming datagrams at random\n"
		"  -X secs:WxH   after this long, report a display change to this size (a rotation), exercising RECONFIGURE\n"
		"  -j SCRIPT     play these touches to the PC (after a second): tap:X,Y swipe:X1,Y1:X2,Y2 pinch:CX,CY:R1:R2 hold:MS, X and Y\n"
		"                as fractions of the screen\n"
		"  -J secs       start the touch script this long after the stream starts (default 1)\n"
		"  -q pct        drop this percentage of the INPUT datagrams we send (tests the repeat-until-acknowledged stream)\n"
		"  -i DIR        identity and known_pcs directory (default ~/.config/pdisp-view)\n"
		"  -I            INSECURE: no handshake, no encryption (only for a daemon started with -I)\n"
		"  -K us         skew this viewer's clock by this many microseconds (tests the clock sync)\n"
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
	const char *identity_dir = NULL, *touch_script = NULL;
	double input_drop_pct = 0, touch_delay = 1;
	bool insecure = false;
	double change_after = 0;
	unsigned change_w = 0, change_h = 0;
	while ((opt = getopt(argc, argv, "W:H:r:D:S:o:cGwL:K:X:i:j:J:q:Ivh")) != -1) {
		switch (opt) {
		case 'W': nw = (unsigned)atoi(optarg); break;
		case 'H': nh = (unsigned)atoi(optarg); break;
		case 'r': rates = optarg; break;
		case 'D': dpi = (unsigned)atoi(optarg); break;
		case 'S': secs = atof(optarg); break;
		case 'o': snap = optarg; V.snap_path = optarg; break;
		case 'c': check = true; break;
		case 'G': V.read_stamps = true; break;
		case 'w': V.window = true; break;
		case 'L': drop_pct = atof(optarg); break;
		case 'K': V.skew_us = atoll(optarg); break;
		case 'i': identity_dir = optarg; break;
		case 'j': touch_script = optarg; break;
		case 'q': input_drop_pct = atof(optarg); break;
		case 'J': touch_delay = atof(optarg); break;
		case 'I': insecure = true; break;
		case 'X':
			if (sscanf(optarg, "%lf:%ux%u", &change_after, &change_w, &change_h) != 3) { usage(argv[0]); return 2; }
			break;
		case 'v': V.verbose = true; break;
		default: usage(argv[0]); return 2;
		}
	}
	struct sigaction sa = { .sa_handler = on_signal };
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	t_start = net_now_us();
	V.drop_rate = drop_pct / 100.0;
	input_tx_init(&V.tx);
	if (touch_script && parse_touch_script(touch_script) != 0) { fprintf(stderr, "cannot parse the touch script '%s'\n", touch_script); return 2; }
	V.drop_rng = 0x2545f4914f6cdd1dull;

	/* ---- control channel: HELLO, SETUP */
	int cfd = net_tcp_connect(host, port);
	if (cfd < 0) { fprintf(stderr, "cannot connect to %s:%u: %s\n", host, port, strerror(-cfd)); return 1; }
	struct net_ctl ctl;
	net_ctl_init(&ctl, cfd);

	struct sec_udp udp;
	struct hs_peer pc;
	char pcs_path[1100] = "";
	bool new_pc = false;
	if (insecure) {
		LOG("WARNING: no authentication or encryption (-I)");
	} else {
		char dir[900];
		if (identity_dir) snprintf(dir, sizeof dir, "%s", identity_dir);
		else {
			char base[800];
			if (identity_default_dir(base, sizeof base) != 0) { fprintf(stderr, "no config directory: use -i DIR\n"); return 1; }
			snprintf(dir, sizeof dir, "%s-view", base);
		}
		struct identity id;
		int created, e = identity_load_or_create(dir, &id, &created);
		if (e) { fprintf(stderr, "cannot use the identity in %s: %s\n", dir, strerror(-e)); return 1; }
		char fp[IDENTITY_FINGERPRINT_LEN];
		identity_fingerprint(id.pub, fp);
		LOG("%s identity in %s, key %s", created ? "created a new" : "using the", dir, fp);
		e = hs_initiate(&ctl, &id, "pdisp-view", 10000, &pc, &udp);
		if (e) { fprintf(stderr, "the secure handshake failed: %s\n", strerror(-e)); return 4; }
		snprintf(pcs_path, sizeof pcs_path, "%s/known_pcs", dir);
		char pcfp[IDENTITY_FINGERPRINT_LEN];
		identity_fingerprint(pc.pub, pcfp);
		new_pc = keystore_find(pcs_path, pc.pub, NULL, 0) <= 0;
		LOG("PC '%s' key %s%s, code %06u", pc.name, pcfp, new_pc ? " (not paired before)" : "", pc.sas);
		printf("PAIRING code=%06u pc_key=%s new_pc=%d\n", pc.sas, pcfp, new_pc);
		fflush(stdout);
	}

	struct proto_hello hello;
	memset(&hello, 0, sizeof hello);
	hello.proto_min = hello.proto_max = PROTO_VERSION;
	snprintf(hello.device_name, sizeof hello.device_name, "pdisp-view");
	hello.device_caps = PROTO_CAP_TOUCH | PROTO_CAP_STYLUS;
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
	if (r == 1 && type == PROTO_CTL_PAIRING_REQUIRED) { /* the PC asks its user: show the code to compare, wait longer */
		LOG("this device is not authorised yet: approve it on the PC, the code there must be %06u", pc.sas);
		printf("PAIRING_REQUIRED code=%06u\n", pc.sas);
		fflush(stdout);
		r = net_ctl_recv(&ctl, &type, &payload, &plen, 120000);
	}
	if (r != 1) { fprintf(stderr, "no answer to HELLO (%d)\n", r); return 1; }
	if (type == PROTO_CTL_REJECT) {
		struct proto_reject rj;
		if (proto_reject_decode(payload, plen, &rj) == 0) fprintf(stderr, "rejected (%u): %s\n", rj.reason, rj.text);
		return rj.reason == 2 || rj.reason == 5 || rj.reason == 6 ? 5 : 3;
	}
	struct proto_setup su;
	if (type != PROTO_CTL_SETUP || proto_setup_decode(payload, plen, &su) != 0) { fprintf(stderr, "bad SETUP\n"); return 1; }
	if (new_pc && keystore_add(pcs_path, pc.pub, pc.name) == 0) LOG("remembered this PC");
	LOG("SETUP: %ux%u@%.3f, session %08x, %u kbit/s, %u bytes of parameter sets", su.params.width, su.params.height,
	    su.params.refresh_mhz / 1000.0, su.session_id, su.params.bitrate_kbps, su.params.param_sets_len);

	if (V.window && SDL_Init(SDL_INIT_VIDEO) != 0) { fprintf(stderr, "cannot initialise SDL: %s\n", SDL_GetError()); return 1; }

	/* ---- decoder */
	if (start_decoder(su.params.param_sets, su.params.param_sets_len) != 0) { fprintf(stderr, "cannot open the HEVC decoder\n"); return 1; }

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
	ufd_global = ufd;
	udp_to = to;
	udp_ctx = insecure ? NULL : &udp;
	uint8_t hb[PROTO_PKT_HDR_SIZE];
	/* every datagram of ours comes from one sequence space: it is the AEAD nonce, so two counters would reuse nonces */
	int hn = vrecv_make_packet(vr, PROTO_PKT_HELLO_UDP, NULL, 0, hb, sizeof hb);
	if (hn > 0) udp_out(hb, (size_t)hn);
	net_ctl_send(&ctl, PROTO_CTL_READY, NULL, 0);
	LOG("sent READY");

	/* ---- receive */
	uint64_t next_fb = net_now_us() + 10000, next_hello = net_now_us() + 100000, next_time = 0;
	timesync_init(&V.ts);
	uint64_t stream_start = 0, end_us = 0, next_ping = 0, change_at = 0;
	uint64_t last_pong = net_now_us();
	uint8_t pkt[2048];
	while (!stop) {
		uint64_t now = net_now_us();
		if (end_us && now >= end_us) break;
		struct pollfd p[2] = { { .fd = ufd, .events = POLLIN }, { .fd = cfd, .events = POLLIN } };
		poll(p, 2, 2);
		now = net_now_us();
		if (V.window) { /* window events and the newest picture, without ever waiting for vsync */
			SDL_Event ev;
			while (SDL_PollEvent(&ev))
				if (ev.type == SDL_QUIT || (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE)) stop = 1;
			show_picture();
		}
		for (;;) {
			ssize_t got = recv(ufd, pkt, sizeof pkt, 0);
			if (got < 0) break;
			if (!insecure) { /* only authenticated, fresh datagrams reach the protocol code */
				int o = sec_udp_open(&udp, pkt, (size_t)got);
				if (o < 0) { V.auth_drops++; continue; }
				got = o;
			}
			if (V.drop_rate > 0) {
				V.drop_rng ^= V.drop_rng << 13; V.drop_rng ^= V.drop_rng >> 7; V.drop_rng ^= V.drop_rng << 17;
				if ((double)(V.drop_rng >> 16 & 0xfffff) / 1048576.0 < V.drop_rate) continue;
			}
			if (!stream_start) {
				stream_start = now;
				end_us = now + (uint64_t)(secs * 1e6);
				if (change_after > 0) change_at = now + (uint64_t)(change_after * 1e6);
				LOG("first packet from the daemon");
			}
			struct proto_pkt_hdr ph;
			if (got >= PROTO_PKT_HDR_SIZE + 24 && proto_pkt_hdr_read(pkt, (size_t)got, &ph) == 0 && ph.type == PROTO_PKT_TIME_RESP) {
				struct proto_time_resp tr;
				uint64_t t4 = cnow();
				if (proto_time_resp_decode(pkt + PROTO_PKT_HDR_SIZE, (size_t)got - PROTO_PKT_HDR_SIZE, &tr) == 0) {
					timesync_add(&V.ts, tr.t1, tr.t2, tr.t3, t4);
					int64_t off, rtt;
					if (timesync_estimate(&V.ts, &off, &rtt)) {
						atomic_store(&V.offset_us, off);
						atomic_store(&V.have_offset, true);
						V.sync_rtt_us = rtt;
					}
					V.time_resp++;
				}
			}
			if (got >= PROTO_PKT_HDR_SIZE + 4 && proto_pkt_hdr_read(pkt, (size_t)got, &ph) == 0 && ph.type == PROTO_PKT_INPUT_ACK) {
				uint32_t acked;
				if (proto_u32_decode(pkt + PROTO_PKT_HDR_SIZE, (size_t)got - PROTO_PKT_HDR_SIZE, &acked) == 0) {
					input_tx_ack(&V.tx, acked);
					if ((int32_t)(acked - (uint32_t)V.touch_acked_to) > 0) V.touch_acked_to = acked;
				}
				continue;
			}
			vrecv_on_packet(vr, pkt, (size_t)got, now);
		}
		if (stream_start && now >= next_time) { /* ask for the PC's clock once a second (every 100 ms at first, to settle quickly) */
			next_time = now + (V.time_resp < 5 ? 100000 : 1000000);
			uint8_t tb[8], tp[PROTO_PKT_HDR_SIZE + 8];
			if (proto_time_req_encode(cnow(), tb, sizeof tb) == 8) {
				int tn = vrecv_make_packet(vr, PROTO_PKT_TIME_REQ, tb, 8, tp, sizeof tp);
				if (tn > 0) udp_out(tp, (size_t)tn);
			}
		}
		if (p[1].revents & (POLLIN | POLLHUP)) {
			bool done = false;
			for (;;) {
				r = net_ctl_recv(&ctl, &type, &payload, &plen, 0);
				if (r == 0) break;
				if (r < 0) { LOG("control connection closed"); done = true; break; }
				if (type == PROTO_CTL_CLOSE) { LOG("the daemon closed the session"); done = true; break; }
				if (type == PROTO_CTL_PING) net_ctl_send(&ctl, PROTO_CTL_PONG, payload, plen);
				else if (type == PROTO_CTL_PONG) { V.pongs++; last_pong = now; }
				else if (type == PROTO_CTL_RECONFIGURE) {
					struct proto_stream_params np;
					if (proto_stream_params_decode(payload, plen, &np) != 0) { LOG("bad RECONFIGURE"); done = true; break; }
					LOG("RECONFIGURE: epoch %u, %ux%u: restarting the decoder", np.epoch, np.width, np.height);
					stop_decoder();
					if (start_decoder(np.param_sets, np.param_sets_len) != 0) { LOG("cannot reopen the decoder"); done = true; break; }
					vrecv_destroy(vr);
					struct vrecv_config nrc = { .session_id = su.session_id, .shard_size = np.shard_size };
					vr = vrecv_create(&nrc, &rcb);
					vrecv_set_epoch(vr, np.epoch);
					if (!insecure) sec_udp_set_epoch(&udp, np.epoch); /* sequence numbers restart: new datagram keys */
					V.reconfigs++;
					V.width = np.width; V.height = np.height; /* until a picture of the new size is decoded */
					net_ctl_send(&ctl, PROTO_CTL_RECONFIGURE_ACK, NULL, 0);
				}
			}
			if (done) break;
		}
		if (stream_start && now >= next_ping) { /* liveness of the control channel */
			next_ping = now + 1000000;
			uint8_t tb[8];
			if (proto_token_encode(cnow(), tb, sizeof tb) == 8) { net_ctl_send(&ctl, PROTO_CTL_PING, tb, 8); V.pings++; }
		}
		if (V.pings > 2 && !V.pongs && now - stream_start > 6000000) { LOG("no PONG from the daemon: the control channel is dead"); break; }
		if (V.pongs && now - last_pong > 6000000) { LOG("no PONG for 6 s: the control channel is dead"); break; }
		if (change_at && now >= change_at) { /* test hook (-X): tell the daemon the display changed, as a rotation would */
			change_at = 0;
			struct proto_hello nh = hello;
			nh.native_width = (uint16_t)change_w; nh.native_height = (uint16_t)change_h;
			nh.orientation = change_h > change_w ? 1 : 0;
			uint8_t nb[2048];
			int nn = proto_hello_encode(&nh, nb, sizeof nb);
			if (nn > 0 && net_ctl_send(&ctl, PROTO_CTL_DISPLAY_CHANGED, nb, (size_t)nn) == 0) LOG("sent DISPLAY_CHANGED: %ux%u", change_w, change_h);
		}
		if (atomic_exchange(&V.decoder_failed, false)) vrecv_decoder_error(vr);
		vrecv_tick(vr, now);
		if (!stream_start && now >= next_hello) { /* until the stream starts, keep telling the daemon where we are */
			next_hello = now + 100000;
			hn = vrecv_make_packet(vr, PROTO_PKT_HELLO_UDP, NULL, 0, hb, sizeof hb);
			if (hn > 0) udp_out(hb, (size_t)hn);
		}
		if (V.n_sched && stream_start) { /* the touch script: events become due one second after the stream starts */
			if (!V.touch_started_us) V.touch_started_us = stream_start + (uint64_t)(touch_delay * 1e6);
			while (V.i_sched < V.n_sched && now >= V.touch_started_us + V.sched[V.i_sched].at_us) {
				struct proto_input_event e = V.sched[V.i_sched++].ev;
				e.time_us = (uint32_t)now;
				input_tx_push(&V.tx, e);
				V.touch_pushed++;
			}
		}
		{
			uint8_t ib[PROTO_MAX_INPUT_EVENTS * 24 + 16], ip[sizeof ib + PROTO_PKT_HDR_SIZE];
			int in_n = input_tx_build(&V.tx, now, 12000, ib, sizeof ib);
			if (in_n > 0) {
				int pn = vrecv_make_packet(vr, PROTO_PKT_INPUT, ib, (size_t)in_n, ip, sizeof ip);
				V.drop_rng ^= V.drop_rng << 13; V.drop_rng ^= V.drop_rng >> 7; V.drop_rng ^= V.drop_rng << 17;
				int lost = (double)(V.drop_rng >> 16 & 0xfffff) / 1048576.0 * 100.0 < input_drop_pct;
				if (pn > 0 && !lost) udp_out(ip, (size_t)pn);
			}
		}
		if (now >= next_fb) {
			next_fb = now + 10000;
			int fn = vrecv_feedback_packet(vr, now, pkt, sizeof pkt);
			if (fn > 0) udp_out(pkt, (size_t)fn);
		}
	}

	/* ---- report: stop the decoder thread first, then convert the newest picture for the checks */
	stop_decoder();
	if (V.tex) SDL_DestroyTexture(V.tex);
	if (V.ren) SDL_DestroyRenderer(V.ren);
	if (V.win) SDL_DestroyWindow(V.win);
	if (V.window) SDL_Quit();
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
	       "decoder_errors=%llu match=%.1f%% freeze_ms=%.1f checks=%u check_min=%.1f%% check_bad=%u check_other=%u clock_offset_us=%lld clock_rtt_ms=%.1f reconfigs=%llu pongs=%llu auth_drops=%llu touch_sent=%llu touch_acked=%llu final_size=%dx%d\n",
	       (unsigned long long)V.decoded, V.decoded / span, V.decoded ? V.decode_ms_sum / V.decoded : 0.0,
	       V.n_lat ? V.lat_us[V.n_lat / 2] / 1000.0 : 0.0, V.n_lat ? V.lat_us[(size_t)((V.n_lat - 1) * 0.99)] / 1000.0 : 0.0,
	       V.n_lat ? V.lat_us[V.n_lat - 1] / 1000.0 : 0.0, (unsigned long long)vs->frames_given_up,
	       (unsigned long long)vs->idr_decoded, (unsigned long long)vs->decoder_errors, match * 100, V.max_gap_us / 1000.0,
	       V.chk_samples, V.chk_samples ? V.chk_min * 100 : 100.0, V.chk_bad, V.chk_other, (long long)atomic_load(&V.offset_us), V.sync_rtt_us / 1000.0,
	       (unsigned long long)V.reconfigs, (unsigned long long)V.pongs, (unsigned long long)V.auth_drops, (unsigned long long)V.touch_pushed, (unsigned long long)V.touch_acked_to, V.last ? V.last->width : 0, V.last ? V.last->height : 0);
	if (V.read_stamps) {
		qsort(V.glass_us, V.n_glass, sizeof *V.glass_us, cmp_u64);
		printf("GLASS stamps=%zu of %llu frames, ms p50=%.1f p99=%.1f max=%.1f\n", V.n_glass, (unsigned long long)V.decoded,
		       V.n_glass ? V.glass_us[V.n_glass / 2] / 1000.0 : 0.0,
		       V.n_glass ? V.glass_us[(size_t)((V.n_glass - 1) * 0.99)] / 1000.0 : 0.0, V.n_glass ? V.glass_us[V.n_glass - 1] / 1000.0 : 0.0);
	}
	if (snap && V.bgra && png_write_bgra(snap, V.bgra, V.width, V.height, V.width * 4) == 0) LOG("wrote %s", snap);

	uint8_t cb[64];
	struct proto_close cl;
	memset(&cl, 0, sizeof cl);
	int cn = proto_close_encode(&cl, cb, sizeof cb);
	if (cn > 0) net_ctl_send(&ctl, PROTO_CTL_CLOSE, cb, (size_t)cn);
	int status = 0;
	if (V.decoded == 0) { fprintf(stderr, "FAIL: nothing was decoded\n"); status = 1; }
	else if (check && match < 0.05)
		fprintf(stderr, "NOTE: the last picture is not the test pattern at all (%.1f%%): the compositor was showing something else\n", match * 100);
	else if (check && match < 0.90) { fprintf(stderr, "FAIL: the decoded picture does not match the pattern (%.1f%%)\n", match * 100); status = 1; }
	vrecv_destroy(vr);
	net_ctl_close(&ctl);
	close(ufd);
	return status;
}
