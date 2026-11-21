/* One streaming session on the phone: TCP handshake, UDP receive with loss repair (the shared proto library), and hardware
 * HEVC decode with AMediaCodec straight onto the SurfaceView's window. Runs on its own thread; Kotlin only starts, stops and
 * reads a status line. */
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <arpa/inet.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <errno.h>
#include <jni.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <time.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "net.h"
#include "proto.h"
#include "timesync.h"
#include "usbmux.h"
#include "vrecv.h"

#define TAG "pdisp"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

struct session {
	pthread_t thread;
	atomic_bool stop, finished;
	ANativeWindow *window;
	char host[128], name[PROTO_NAME_MAX];
	atomic_uint pending_w, pending_h; /* a new display size reported by the UI thread, sent by the session thread */
	int usb_fd; /* >= 0: talk over this USB accessory descriptor instead of the network */
	unsigned port, width, height, dpi, n_rates, rates_mhz[PROTO_MAX_RATES];

	pthread_mutex_t status_mu;
	char status[256];

	AMediaCodec *codec;
	atomic_bool decoder_failed;
	uint64_t decoded, rendered, input_refused, dropped_in_drain;
	uint64_t frame_ns, next_slot_ns; /* the presentation grid: one slot per frame interval */

	/* render cadence: intervals between presented pictures, for the periodic report */
	uint64_t last_render_us;
	uint32_t intervals_us[1024];
	unsigned n_intervals;
	unsigned long over25, over40, over70;
	/* the same for complete frames reaching the decoder: separates network jitter from decode delay */
	/* decode latency: time from queueing a frame to its picture being ready, keyed by presentation timestamp */
	uint64_t in_time_us[64], in_pts[64];
	uint32_t dec_us[1024];
	unsigned n_dec;
	/* clock sync with the PC and the end-to-end latency it makes measurable: PC capture time to the picture being ready and to the
	 * vsync it was scheduled for */
	struct timesync ts;
	atomic_llong offset_us; /* server_time = phone_time + offset */
	atomic_bool have_offset;
	int64_t sync_rtt_us;
	uint64_t time_resp;
	uint32_t lat_ready_us[1024], lat_present_us[1024];
	unsigned n_lat;
	uint64_t last_arrival_us;
	uint32_t arrivals_us[1024];
	unsigned n_arrivals;
};

static int cmp_u32(const void *a, const void *b)
{
	uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
	return x < y ? -1 : x > y;
}

static void report_cadence(struct session *s)
{
	if (s->n_intervals < 10) return;
	qsort(s->intervals_us, s->n_intervals, sizeof s->intervals_us[0], cmp_u32);
	unsigned n = s->n_intervals;
	LOGI("CADENCE n=%u interval_ms p50=%.1f p95=%.1f p99=%.1f max=%.1f  over25=%lu over40=%lu over70=%lu  dropped_in_drain=%llu refused=%llu",
	     n, s->intervals_us[n / 2] / 1000.0, s->intervals_us[n * 95 / 100] / 1000.0, s->intervals_us[n * 99 / 100] / 1000.0,
	     s->intervals_us[n - 1] / 1000.0, s->over25, s->over40, s->over70, (unsigned long long)s->dropped_in_drain,
	     (unsigned long long)s->input_refused);
	if (s->n_arrivals >= 10) {
		qsort(s->arrivals_us, s->n_arrivals, sizeof s->arrivals_us[0], cmp_u32);
		unsigned m = s->n_arrivals;
		LOGI("ARRIVAL n=%u frame_gap_ms p50=%.1f p95=%.1f p99=%.1f max=%.1f", m, s->arrivals_us[m / 2] / 1000.0,
		     s->arrivals_us[m * 95 / 100] / 1000.0, s->arrivals_us[m * 99 / 100] / 1000.0, s->arrivals_us[m - 1] / 1000.0);
	}
	if (s->n_dec >= 10) {
		qsort(s->dec_us, s->n_dec, sizeof s->dec_us[0], cmp_u32);
		unsigned m = s->n_dec;
		LOGI("DECODE n=%u latency_ms p50=%.1f p95=%.1f p99=%.1f max=%.1f", m, s->dec_us[m / 2] / 1000.0,
		     s->dec_us[m * 95 / 100] / 1000.0, s->dec_us[m * 99 / 100] / 1000.0, s->dec_us[m - 1] / 1000.0);
	}
	if (s->n_lat >= 10) {
		qsort(s->lat_ready_us, s->n_lat, sizeof s->lat_ready_us[0], cmp_u32);
		qsort(s->lat_present_us, s->n_lat, sizeof s->lat_present_us[0], cmp_u32);
		unsigned m = s->n_lat;
		LOGI("LATENCY n=%u capture_to_decoded_ms p50=%.1f p95=%.1f p99=%.1f  capture_to_vsync_ms p50=%.1f p95=%.1f p99=%.1f max=%.1f  clock_rtt_ms=%.1f",
		     m, s->lat_ready_us[m / 2] / 1000.0, s->lat_ready_us[m * 95 / 100] / 1000.0, s->lat_ready_us[m * 99 / 100] / 1000.0,
		     s->lat_present_us[m / 2] / 1000.0, s->lat_present_us[m * 95 / 100] / 1000.0, s->lat_present_us[m * 99 / 100] / 1000.0,
		     s->lat_present_us[m - 1] / 1000.0, s->sync_rtt_us / 1000.0);
	}
	s->n_lat = 0;
	s->n_dec = 0;
	s->n_arrivals = 0;
	s->n_intervals = 0;
	s->over25 = s->over40 = s->over70 = 0;
}

static void set_status(struct session *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void set_status(struct session *s, const char *fmt, ...)
{
	pthread_mutex_lock(&s->status_mu);
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(s->status, sizeof s->status, fmt, ap);
	va_end(ap);
	pthread_mutex_unlock(&s->status_mu);
	LOGI("%s", s->status);
}

static void close_codec(struct session *s)
{
	if (!s->codec) return;
	AMediaCodec_stop(s->codec);
	AMediaCodec_delete(s->codec);
	s->codec = NULL;
}

/* Tries to start a decoder for a stream: the parameter sets go in as codec-specific data, output goes to the window.
 * `hints` asks for real-time scheduling, which some vendors (Exynos) refuse; the caller then retries without. */
static int try_codec(struct session *s, const struct proto_stream_params *p, int hints)
{
	AMediaFormat *f = AMediaFormat_new();
	AMediaFormat_setString(f, AMEDIAFORMAT_KEY_MIME, "video/hevc");
	AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_WIDTH, p->width);
	AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_HEIGHT, p->height);
	AMediaFormat_setBuffer(f, "csd-0", p->param_sets, p->param_sets_len);
	AMediaFormat_setInt32(f, "low-latency", 1); /* no output reordering delay */
	if (hints & 1) AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_PRIORITY, 0); /* real-time */
	if (hints & 2) AMediaFormat_setInt32(f, "operating-rate", 120);        /* clocks the decoder for this many frames a second */
	s->codec = AMediaCodec_createDecoderByType("video/hevc");
	if (!s->codec) { AMediaFormat_delete(f); LOGE("no HEVC decoder"); return -1; }
	media_status_t st = AMediaCodec_configure(s->codec, f, s->window, NULL, 0);
	AMediaFormat_delete(f);
	if (st == AMEDIA_OK) st = AMediaCodec_start(s->codec);
	if (st != AMEDIA_OK) { LOGE("cannot start the decoder (hints %d): %d", hints, (int)st); close_codec(s); return -1; }
	return 0;
}

static int open_codec(struct session *s, const struct proto_stream_params *p)
{
	close_codec(s);
	/* vendors accept different subsets of the real-time hints (Exynos refuses the priority): take the most we can get */
	static const int order[] = { 3, 2, 1, 0 };
	int ok = -1;
	for (unsigned i = 0; i < sizeof order / sizeof order[0] && ok < 0; i++)
		if (try_codec(s, p, order[i]) == 0) ok = order[i];
	if (ok < 0) return -1;
	LOGI("decoder started with hints %d (1 = priority, 2 = operating rate)", ok);
	atomic_store(&s->decoder_failed, false);
	s->frame_ns = p->refresh_mhz ? 1000000000000ull / p->refresh_mhz : 16666667;
	s->next_slot_ns = 0;
	{ /* run the panel at the stream's rate, not a 90 Hz multiple that judders (API 30, so looked up at run time) */
		typedef int32_t (*set_frame_rate_fn)(ANativeWindow *, float, int8_t);
		set_frame_rate_fn fn = (set_frame_rate_fn)dlsym(RTLD_DEFAULT, "ANativeWindow_setFrameRate");
		if (fn) fn(s->window, (float)p->refresh_mhz / 1000.0f, 1 /* FIXED_SOURCE */);
	}
	return 0;
}

static void drain_output(struct session *s);

/* vrecv's decode callback: copy the access unit into a free codec input buffer. Non-zero means the decoder could not take it,
 * which makes the receiver ask for an IDR. */
static int decode_cb(const struct vrecv_frame *f, void *user)
{
	struct session *s = user;
	if (!s->codec || atomic_load(&s->decoder_failed)) return -1;
	{
		uint64_t t = net_now_us();
		if (s->last_arrival_us && s->n_arrivals < 1024) {
			uint64_t d = t - s->last_arrival_us;
			s->arrivals_us[s->n_arrivals++] = (uint32_t)(d > 4000000 ? 4000000 : d);
		}
		s->last_arrival_us = t;
	}
	/* A hardware decoder has only a few input buffers. Refusing a frame breaks the reference chain and costs a keyframe, so wait
	 * a little (showing finished pictures meanwhile, which is what frees buffers) before giving up. */
	ssize_t idx = AMediaCodec_dequeueInputBuffer(s->codec, 0);
	/* the first frames after start() may find the decoder still warming up: give them a lot longer than steady state */
	for (int i = 0, tries = s->decoded < 8 ? 60 : 6; idx < 0 && i < tries; i++) {
		drain_output(s);
		idx = AMediaCodec_dequeueInputBuffer(s->codec, 1000);
	}
	if (idx < 0) { s->input_refused++; return -1; }
	size_t cap = 0;
	uint8_t *buf = AMediaCodec_getInputBuffer(s->codec, (size_t)idx, &cap);
	if (!buf || cap < f->size) { AMediaCodec_queueInputBuffer(s->codec, (size_t)idx, 0, 0, 0, 0); return -1; }
	memcpy(buf, f->data, f->size);
	if (AMediaCodec_queueInputBuffer(s->codec, (size_t)idx, 0, f->size, f->capture_us, 0) != AMEDIA_OK) return -1;
	s->in_pts[s->decoded % 64] = f->capture_us;
	s->in_time_us[s->decoded % 64] = net_now_us();
	s->decoded++;
	return 0;
}

/* Presents decoded pictures on a regular grid of display times. A picture that is ready is not shown "now" (the display then
 * takes it at whatever refresh comes next, and a second picture ready in the same moment replaces it): each gets the next free
 * slot, one frame interval after the previous, and SurfaceFlinger shows it at the vsync at or after that time. Only when the
 * backlog is more than one frame deep (the decoder was stalled and caught up) are the oldest pictures dropped. */
static uint64_t mono_ns(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static void drain_output(struct session *s)
{
	if (!s->codec) return;
	AMediaCodecBufferInfo info;
	for (int guard = 0; guard < 16; guard++) {
		ssize_t idx = AMediaCodec_dequeueOutputBuffer(s->codec, &info, 0);
		if (idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED || idx == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) continue;
		if (idx == AMEDIACODEC_INFO_TRY_AGAIN_LATER) break;
		if (idx < 0) {
			LOGE("decoder output error %zd", idx);
			atomic_store(&s->decoder_failed, true);
			break;
		}
		for (unsigned k = 0; k < 64; k++) /* when did this picture go in? */
			if (s->in_pts[k] == (uint64_t)info.presentationTimeUs && s->in_time_us[k]) {
				if (s->n_dec < 1024) s->dec_us[s->n_dec++] = (uint32_t)(net_now_us() - s->in_time_us[k]);
				s->in_time_us[k] = 0;
				break;
			}
		uint64_t now = mono_ns(), step = s->frame_ns ? s->frame_ns : 16666667;
		uint64_t slot = s->next_slot_ns > now ? s->next_slot_ns : now;
		if (slot > now + step + step / 2) { /* more than one frame behind: skip this picture so the grid catches up (latency over smoothness) */
			AMediaCodec_releaseOutputBuffer(s->codec, (size_t)idx, false);
			s->dropped_in_drain++;
			continue;
		}
		AMediaCodec_releaseOutputBufferAtTime(s->codec, (size_t)idx, (int64_t)slot);
		if (atomic_load(&s->have_offset) && s->n_lat < 1024) {
			int64_t off = atomic_load(&s->offset_us), cap = info.presentationTimeUs;
			int64_t ready = (int64_t)net_now_us() + off - cap, vsync = (int64_t)(slot / 1000) + off - cap;
			if (ready > 0 && ready < 5000000 && vsync > 0) {
				s->lat_ready_us[s->n_lat] = (uint32_t)ready;
				s->lat_present_us[s->n_lat++] = (uint32_t)vsync;
			}
		}
		s->next_slot_ns = slot + step;
		s->rendered++;
		uint64_t t = slot / 1000;
		if (s->last_render_us) {
			uint64_t d = t - s->last_render_us;
			if (s->n_intervals < 1024) s->intervals_us[s->n_intervals++] = (uint32_t)(d > 4000000 ? 4000000 : d);
			if (d > 25000) s->over25++;
			if (d > 40000) s->over40++;
			if (d > 70000) s->over70++;
		}
		s->last_render_us = t;
	}
}

static int send_hello(struct session *s, struct net_ctl *ctl, uint16_t type)
{
	struct proto_hello h;
	memset(&h, 0, sizeof h);
	h.proto_min = h.proto_max = PROTO_VERSION;
	snprintf(h.device_name, sizeof h.device_name, "%s", s->name);
	h.device_caps = PROTO_CAP_TOUCH;
	h.native_width = (uint16_t)s->width;
	h.native_height = (uint16_t)s->height;
	h.orientation = s->height > s->width ? 1 : 0;
	h.density_dpi = (uint16_t)s->dpi;
	for (unsigned i = 0; i < s->n_rates && i < PROTO_MAX_RATES; i++) h.refresh_mhz[h.n_refresh++] = s->rates_mhz[i];
	h.n_codecs = 1;
	h.codecs[0] = PROTO_CODEC_HEVC;
	h.max_touch_points = 10;
	h.max_udp_payload = 1472;
	snprintf(h.app_version, sizeof h.app_version, "0.1");
	uint8_t b[512];
	int n = proto_hello_encode(&h, b, sizeof b);
	return n < 0 ? n : net_ctl_send(ctl, type, b, (size_t)n);
}

static void *run(void *arg)
{
	struct session *s = arg;
	int cfd = -1, usb_dgram = -1;
	struct usbmux *mux = NULL;
	if (s->usb_fd >= 0) {
		set_status(s, "connected over USB, waiting for the PC");
		int e = usbmux_start(s->usb_fd, s->usb_fd, USBMUX_LINK_BLOCKING_READ, &mux, &cfd, &usb_dgram);
		if (e) { set_status(s, "cannot start the USB link: %s", strerror(-e)); atomic_store(&s->finished, true); return NULL; }
		fcntl(usb_dgram, F_SETFL, fcntl(usb_dgram, F_GETFL) | O_NONBLOCK);
	} else {
		set_status(s, "connecting to %s:%u", s->host, s->port);
		cfd = net_tcp_connect(s->host, s->port);
		if (cfd < 0) { set_status(s, "cannot connect to %s:%u: %s", s->host, s->port, strerror(-cfd)); atomic_store(&s->finished, true); return NULL; }
		net_tune_tcp(cfd);
	}
	struct net_ctl ctl;
	net_ctl_init(&ctl, cfd);
	struct vrecv *vr = NULL;
	int ufd = -1;

	uint16_t type;
	const uint8_t *payload;
	size_t plen;
	if (send_hello(s, &ctl, PROTO_CTL_HELLO) != 0) { set_status(s, "cannot send HELLO"); goto out; }
	set_status(s, "waiting for the PC to create the monitor (%ux%u)", s->width, s->height);
	int r = 0;
	while (!atomic_load(&s->stop) && (r = net_ctl_recv(&ctl, &type, &payload, &plen, 200)) == 0) {}
	if (atomic_load(&s->stop)) goto out;
	if (r != 1) { set_status(s, "the PC closed the connection"); goto out; }
	if (type == PROTO_CTL_REJECT) {
		struct proto_reject rj;
		if (proto_reject_decode(payload, plen, &rj) == 0) set_status(s, "the PC refused: %s", rj.text);
		else set_status(s, "the PC refused the connection");
		goto out;
	}
	struct proto_setup su;
	if (type != PROTO_CTL_SETUP || proto_setup_decode(payload, plen, &su) != 0) { set_status(s, "bad SETUP from the PC"); goto out; }
	if (open_codec(s, &su.params) != 0) { set_status(s, "this device cannot decode the stream"); goto out; }

	struct vrecv_config rc = { .session_id = su.session_id, .shard_size = su.params.shard_size };
	struct vrecv_callbacks rcb = { .decode = decode_cb, .user = s };
	vr = vrecv_create(&rc, &rcb);
	ufd = s->usb_fd >= 0 ? usb_dgram : net_udp_bind(0);
	if (s->usb_fd < 0) net_tune_udp(ufd);
	{ /* keyframes arrive as bursts of 100+ packets: the default receive buffer overflows */
		int rcvbuf = 4 << 20;
		setsockopt(ufd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf);
		socklen_t ol = sizeof rcvbuf;
		getsockopt(ufd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, &ol);
		LOGI("UDP receive buffer %d bytes", rcvbuf);
	}
	struct sockaddr_in to;
	memset(&to, 0, sizeof to);
	if (s->usb_fd < 0) {
		socklen_t pl = sizeof to;
		getpeername(cfd, (struct sockaddr *)&to, &pl);
		to.sin_port = htons(su.udp_port);
	}
#define DSEND(buf, n) (s->usb_fd >= 0 ? send(ufd, (buf), (n), MSG_DONTWAIT) : sendto(ufd, (buf), (n), 0, (struct sockaddr *)&to, sizeof to))
	uint64_t udp_seq = 0;
	struct proto_pkt_hdr hh = { .session_id = su.session_id, .type = PROTO_PKT_HELLO_UDP };
	uint8_t hb[PROTO_PKT_HDR_SIZE], pkt[2048];
	hh.seq = udp_seq++;
	proto_pkt_hdr_write(&hh, hb, sizeof hb);
	DSEND(hb, sizeof hb);
	net_ctl_send(&ctl, PROTO_CTL_READY, NULL, 0);

	uint64_t t0 = net_now_us(), next_fb = t0 + 10000, next_hello = t0 + 100000, next_ping = t0 + 1000000, last_stat = t0;
	uint64_t last_rx = t0, last_rendered = 0, last_pong = t0, last_cadence = t0, next_time = 0, last_given_up = 0;
	timesync_init(&s->ts);
	bool streaming = false;
	unsigned width = su.params.width, height = su.params.height;
	set_status(s, "connected, waiting for video");
	while (!atomic_load(&s->stop)) {
		struct pollfd p[2] = { { .fd = ufd, .events = POLLIN }, { .fd = cfd, .events = POLLIN } };
		poll(p, 2, 2);
		uint64_t now = net_now_us();
		for (;;) {
			ssize_t got = recv(ufd, pkt, sizeof pkt, 0);
			if (got < 0) break;
			last_rx = now;
			streaming = true;
			struct proto_pkt_hdr ph;
			if (got >= PROTO_PKT_HDR_SIZE + 24 && proto_pkt_hdr_read(pkt, (size_t)got, &ph) == 0 && ph.type == PROTO_PKT_TIME_RESP) {
				struct proto_time_resp tr;
				uint64_t t4 = net_now_us();
				if (proto_time_resp_decode(pkt + PROTO_PKT_HDR_SIZE, (size_t)got - PROTO_PKT_HDR_SIZE, &tr) == 0) {
					timesync_add(&s->ts, tr.t1, tr.t2, tr.t3, t4);
					int64_t off, rtt;
					if (timesync_estimate(&s->ts, &off, &rtt)) {
						atomic_store(&s->offset_us, off);
						atomic_store(&s->have_offset, true);
						s->sync_rtt_us = rtt;
					}
					s->time_resp++;
				}
			}
			vrecv_on_packet(vr, pkt, (size_t)got, now);
		}
		if (p[1].revents & (POLLIN | POLLHUP)) {
			for (;;) {
				r = net_ctl_recv(&ctl, &type, &payload, &plen, 0);
				if (r == 0) break;
				if (r < 0) { set_status(s, "the PC closed the connection"); goto out; }
				if (type == PROTO_CTL_CLOSE) { set_status(s, "the PC ended the session"); goto out; }
				if (type == PROTO_CTL_PING) net_ctl_send(&ctl, PROTO_CTL_PONG, payload, plen);
				else if (type == PROTO_CTL_PONG) last_pong = now;
				else if (type == PROTO_CTL_RECONFIGURE) {
					struct proto_stream_params np;
					if (proto_stream_params_decode(payload, plen, &np) != 0 || open_codec(s, &np) != 0) {
						set_status(s, "cannot follow the PC's display change");
						goto out;
					}
					vrecv_destroy(vr);
					struct vrecv_config nrc = { .session_id = su.session_id, .shard_size = np.shard_size };
					vr = vrecv_create(&nrc, &rcb);
					vrecv_set_epoch(vr, np.epoch);
					width = np.width; height = np.height;
					net_ctl_send(&ctl, PROTO_CTL_RECONFIGURE_ACK, NULL, 0);
				}
			}
		}
		if (atomic_exchange(&s->decoder_failed, false)) {
			/* the codec broke (not just a refused frame): rebuild it and ask for a keyframe */
			struct proto_stream_params cur = su.params;
			cur.width = (uint16_t)width; cur.height = (uint16_t)height;
			if (open_codec(s, &cur) != 0) { set_status(s, "the decoder failed"); goto out; }
			vrecv_decoder_error(vr);
		}
		drain_output(s);
		{ /* the UI thread reported a rotation: tell the PC, which answers with RECONFIGURE */
			unsigned pw = atomic_exchange(&s->pending_w, 0), ph2 = atomic_exchange(&s->pending_h, 0);
			if (pw && ph2 && (pw != s->width || ph2 != s->height)) {
				s->width = pw; s->height = ph2;
				if (send_hello(s, &ctl, PROTO_CTL_DISPLAY_CHANGED) == 0) set_status(s, "display changed to %ux%u, waiting for the PC", pw, ph2);
			}
		}
		vrecv_tick(vr, now);
		{ /* when does the receiver give up on frames? (startup bursts versus steady state) */
			const struct vrecv_stats *gs = vrecv_stats(vr);
			if (gs->frames_given_up != last_given_up) {
				LOGI("GIVEUP total=%llu (+%llu) at %.2f s into the stream, decoded %llu, refused %llu", (unsigned long long)gs->frames_given_up,
				     (unsigned long long)(gs->frames_given_up - last_given_up), (now - t0) / 1e6, (unsigned long long)s->decoded,
				     (unsigned long long)s->input_refused);
				last_given_up = gs->frames_given_up;
			}
		}
		if (!streaming && now >= next_hello) { /* until video arrives keep telling the PC where we are */
			next_hello = now + 100000;
			hh.seq = udp_seq++;
			proto_pkt_hdr_write(&hh, hb, sizeof hb);
			DSEND(hb, sizeof hb);
		}
		if (streaming && now >= next_time) { /* ask for the PC's clock: every 100 ms at first to settle, then once a second */
			next_time = now + (s->time_resp < 5 ? 100000 : 1000000);
			uint8_t tb[8], tp[PROTO_PKT_HDR_SIZE + 8];
			if (proto_time_req_encode(net_now_us(), tb, sizeof tb) == 8) {
				int tn = vrecv_make_packet(vr, PROTO_PKT_TIME_REQ, tb, 8, tp, sizeof tp);
				if (tn > 0) DSEND(tp, (size_t)tn);
			}
		}
		if (now >= next_fb) {
			next_fb = now + 10000;
			int fn = vrecv_feedback_packet(vr, now, pkt, sizeof pkt);
			if (fn > 0) DSEND(pkt, (size_t)fn);
		}
		if (now >= next_ping) {
			next_ping = now + 1000000;
			uint8_t tb[8];
			if (proto_token_encode(now, tb, sizeof tb) == 8) net_ctl_send(&ctl, PROTO_CTL_PING, tb, 8);
		}
		if (now - last_pong > 8000000 && streaming) { set_status(s, "lost contact with the PC"); goto out; }
		if (streaming && now - last_rx > 5000000) { set_status(s, "no data from the PC for 5 s"); goto out; }
		if (now - last_stat >= 500000) {
			double secs = (now - last_stat) / 1e6;
			const struct vrecv_stats *st = vrecv_stats(vr);
			set_status(s, "%ux%u  %.0f fps  decoded %llu  lost frames %llu  IDR requests %llu", width, height,
				   (s->rendered - last_rendered) / secs, (unsigned long long)st->frames_decoded,
				   (unsigned long long)st->frames_given_up, (unsigned long long)st->idr_decoded);
			last_stat = now;
			last_rendered = s->rendered;
			if (now - last_cadence >= 5000000) { last_cadence = now; report_cadence(s); }
		}
	}
	/* tell the PC we are leaving */
	{
		struct proto_close cl;
		memset(&cl, 0, sizeof cl);
		uint8_t cb[64];
		int cn = proto_close_encode(&cl, cb, sizeof cb);
		if (cn > 0) net_ctl_send(&ctl, PROTO_CTL_CLOSE, cb, (size_t)cn);
	}
out:
	close_codec(s);
	if (vr) vrecv_destroy(vr);
	if (ufd >= 0) close(ufd);
	net_ctl_close(&ctl);
	if (mux) usbmux_stop(mux);
	atomic_store(&s->finished, true);
	return NULL;
}

static jlong start_common(JNIEnv *env, jstring host, jint port, jint usb_fd, jobject surface, jint width, jint height, jint dpi,
			  jintArray rates, jstring name)
{
	struct session *s = calloc(1, sizeof *s);
	if (!s) return 0;
	s->usb_fd = usb_fd;
	pthread_mutex_init(&s->status_mu, NULL);
	if (host) {
		const char *h = (*env)->GetStringUTFChars(env, host, NULL);
		snprintf(s->host, sizeof s->host, "%s", h);
		(*env)->ReleaseStringUTFChars(env, host, h);
	}
	const char *n = (*env)->GetStringUTFChars(env, name, NULL);
	snprintf(s->name, sizeof s->name, "%s", n);
	(*env)->ReleaseStringUTFChars(env, name, n);
	s->port = (unsigned)port; s->width = (unsigned)width; s->height = (unsigned)height; s->dpi = (unsigned)dpi;
	jsize nr = (*env)->GetArrayLength(env, rates);
	if (nr > PROTO_MAX_RATES) nr = PROTO_MAX_RATES;
	jint tmp[PROTO_MAX_RATES];
	(*env)->GetIntArrayRegion(env, rates, 0, nr, tmp);
	for (jsize i = 0; i < nr; i++) s->rates_mhz[s->n_rates++] = (unsigned)tmp[i];
	s->window = ANativeWindow_fromSurface(env, surface);
	if (!s->window) { free(s); return 0; }
	if (pthread_create(&s->thread, NULL, run, s) != 0) { ANativeWindow_release(s->window); free(s); return 0; }
	return (jlong)(intptr_t)s;
}

JNIEXPORT jlong JNICALL Java_dev_pdisp_PdispNative_nativeStart(JNIEnv *env, jclass cls, jstring host, jint port, jobject surface,
							      jint width, jint height, jint dpi, jintArray rates, jstring name)
{
	(void)cls;
	return start_common(env, host, port, -1, surface, width, height, dpi, rates, name);
}

/* `fd` is the descriptor of the open USB accessory; ownership passes to the native side. */
JNIEXPORT jlong JNICALL Java_dev_pdisp_PdispNative_nativeStartUsb(JNIEnv *env, jclass cls, jint fd, jobject surface, jint width,
								 jint height, jint dpi, jintArray rates, jstring name)
{
	(void)cls;
	return start_common(env, NULL, 0, fd, surface, width, height, dpi, rates, name);
}

JNIEXPORT void JNICALL Java_dev_pdisp_PdispNative_nativeStop(JNIEnv *env, jclass cls, jlong handle)
{
	(void)env; (void)cls;
	struct session *s = (struct session *)(intptr_t)handle;
	if (!s) return;
	atomic_store(&s->stop, true);
	pthread_join(s->thread, NULL);
	ANativeWindow_release(s->window);
	free(s);
}

JNIEXPORT jstring JNICALL Java_dev_pdisp_PdispNative_nativeStatus(JNIEnv *env, jclass cls, jlong handle)
{
	(void)cls;
	struct session *s = (struct session *)(intptr_t)handle;
	char copy[256] = "";
	if (s) {
		pthread_mutex_lock(&s->status_mu);
		snprintf(copy, sizeof copy, "%s", s->status);
		pthread_mutex_unlock(&s->status_mu);
	}
	return (*env)->NewStringUTF(env, copy);
}

JNIEXPORT jboolean JNICALL Java_dev_pdisp_PdispNative_nativeFinished(JNIEnv *env, jclass cls, jlong handle)
{
	(void)env; (void)cls;
	struct session *s = (struct session *)(intptr_t)handle;
	return !s || atomic_load(&s->finished);
}

JNIEXPORT void JNICALL Java_dev_pdisp_PdispNative_nativeDisplayChanged(JNIEnv *env, jclass cls, jlong handle, jint width, jint height)
{
	(void)env; (void)cls;
	struct session *s = (struct session *)(intptr_t)handle;
	if (!s || width <= 0 || height <= 0) return;
	atomic_store(&s->pending_h, (unsigned)height);
	atomic_store(&s->pending_w, (unsigned)width);
}
