/* One streaming session on the phone: TCP handshake, UDP receive with loss repair (the shared proto library), and hardware
 * HEVC decode with AMediaCodec straight onto the SurfaceView's window. Runs on its own thread; Kotlin only starts, stops and
 * reads a status line. */
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <arpa/inet.h>
#include <errno.h>
#include <jni.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "net.h"
#include "proto.h"
#include "vrecv.h"

#define TAG "pdisp"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

struct session {
	pthread_t thread;
	atomic_bool stop;
	ANativeWindow *window;
	char host[128], name[PROTO_NAME_MAX];
	unsigned port, width, height, dpi, n_rates, rates_mhz[PROTO_MAX_RATES];

	pthread_mutex_t status_mu;
	char status[256];

	AMediaCodec *codec;
	atomic_bool decoder_failed;
	uint64_t decoded, rendered, input_refused;
};

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

/* Creates the decoder for a stream: the parameter sets go in as codec-specific data, output goes to the window. */
static int open_codec(struct session *s, const struct proto_stream_params *p)
{
	close_codec(s);
	AMediaFormat *f = AMediaFormat_new();
	AMediaFormat_setString(f, AMEDIAFORMAT_KEY_MIME, "video/hevc");
	AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_WIDTH, p->width);
	AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_HEIGHT, p->height);
	AMediaFormat_setBuffer(f, "csd-0", p->param_sets, p->param_sets_len);
	AMediaFormat_setInt32(f, "low-latency", 1);            /* no output reordering delay */
	AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_PRIORITY, 0); /* real-time */
	AMediaFormat_setInt32(f, "operating-rate", 120);
	s->codec = AMediaCodec_createDecoderByType("video/hevc");
	if (!s->codec) { AMediaFormat_delete(f); LOGE("no HEVC decoder"); return -1; }
	media_status_t st = AMediaCodec_configure(s->codec, f, s->window, NULL, 0);
	AMediaFormat_delete(f);
	if (st == AMEDIA_OK) st = AMediaCodec_start(s->codec);
	if (st != AMEDIA_OK) { LOGE("cannot start the decoder: %d", (int)st); close_codec(s); return -1; }
	atomic_store(&s->decoder_failed, false);
	return 0;
}

/* vrecv's decode callback: copy the access unit into a free codec input buffer. Non-zero means the decoder could not take it,
 * which makes the receiver ask for an IDR. */
static int decode_cb(const struct vrecv_frame *f, void *user)
{
	struct session *s = user;
	if (!s->codec || atomic_load(&s->decoder_failed)) return -1;
	ssize_t idx = AMediaCodec_dequeueInputBuffer(s->codec, 0);
	if (idx < 0) { s->input_refused++; return -1; }
	size_t cap = 0;
	uint8_t *buf = AMediaCodec_getInputBuffer(s->codec, (size_t)idx, &cap);
	if (!buf || cap < f->size) { AMediaCodec_queueInputBuffer(s->codec, (size_t)idx, 0, 0, 0, 0); return -1; }
	memcpy(buf, f->data, f->size);
	if (AMediaCodec_queueInputBuffer(s->codec, (size_t)idx, 0, f->size, f->capture_us, 0) != AMEDIA_OK) return -1;
	s->decoded++;
	return 0;
}

/* Shows the newest decoded picture; older ones that are ready are dropped, never queued up behind it. */
static void drain_output(struct session *s)
{
	if (!s->codec) return;
	AMediaCodecBufferInfo info;
	ssize_t pending = -1;
	for (int guard = 0; guard < 16; guard++) {
		ssize_t idx = AMediaCodec_dequeueOutputBuffer(s->codec, &info, 0);
		if (idx >= 0) {
			if (pending >= 0) AMediaCodec_releaseOutputBuffer(s->codec, (size_t)pending, false);
			pending = idx;
		} else if (idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED || idx == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
			continue;
		} else if (idx == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
			break;
		} else {
			LOGE("decoder output error %zd", idx);
			atomic_store(&s->decoder_failed, true);
			break;
		}
	}
	if (pending >= 0) {
		AMediaCodec_releaseOutputBuffer(s->codec, (size_t)pending, true);
		s->rendered++;
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
	set_status(s, "connecting to %s:%u", s->host, s->port);
	int cfd = net_tcp_connect(s->host, s->port);
	if (cfd < 0) { set_status(s, "cannot connect to %s:%u: %s", s->host, s->port, strerror(-cfd)); return NULL; }
	net_tune_tcp(cfd);
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
	ufd = net_udp_bind(0);
	net_tune_udp(ufd);
	struct sockaddr_in to;
	{
		socklen_t pl = sizeof to;
		getpeername(cfd, (struct sockaddr *)&to, &pl);
		to.sin_port = htons(su.udp_port);
	}
	uint64_t udp_seq = 0;
	struct proto_pkt_hdr hh = { .session_id = su.session_id, .type = PROTO_PKT_HELLO_UDP };
	uint8_t hb[PROTO_PKT_HDR_SIZE], pkt[2048];
	hh.seq = udp_seq++;
	proto_pkt_hdr_write(&hh, hb, sizeof hb);
	sendto(ufd, hb, sizeof hb, 0, (struct sockaddr *)&to, sizeof to);
	net_ctl_send(&ctl, PROTO_CTL_READY, NULL, 0);

	uint64_t t0 = net_now_us(), next_fb = t0 + 10000, next_hello = t0 + 100000, next_ping = t0 + 1000000, last_stat = t0;
	uint64_t last_rx = t0, last_rendered = 0, last_pong = t0;
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
		vrecv_tick(vr, now);
		if (!streaming && now >= next_hello) { /* until video arrives keep telling the PC where we are */
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
	return NULL;
}

JNIEXPORT jlong JNICALL Java_dev_pdisp_PdispNative_nativeStart(JNIEnv *env, jclass cls, jstring host, jint port, jobject surface,
							      jint width, jint height, jint dpi, jintArray rates, jstring name)
{
	(void)cls;
	struct session *s = calloc(1, sizeof *s);
	if (!s) return 0;
	pthread_mutex_init(&s->status_mu, NULL);
	const char *h = (*env)->GetStringUTFChars(env, host, NULL);
	snprintf(s->host, sizeof s->host, "%s", h);
	(*env)->ReleaseStringUTFChars(env, host, h);
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
