/* pdispd: the pdisp daemon. Accepts one phone on TCP, creates a virtual monitor sized for it, then captures the monitor,
 * encodes it and streams it over UDP with loss repair (docs/protocol.md).
 *
 *   pdispd [-p PORT] [-n NAME] [-t SERIAL] [-m WxH@HZ] [-R MAXHZ] [-b KBPS] [-B MAXKBPS] [-r NODE] [-d CARD] [-S SECS]
 *
 * Exit: 0 after the phone left or -S elapsed, 1 on error, 3 if there is no free evdi device.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "edid.h"
#include "encoder.h"
#include "net.h"
#include "proto.h"
#include "vdisplay.h"
#include "video.h"
#include "vsend.h"

static volatile sig_atomic_t stop;
static void on_signal(int sig) { (void)sig; stop = 1; }

static uint64_t t_start;
#define LOG(...) do { fprintf(stderr, "[%8.3f] ", (net_now_us() - t_start) / 1e6); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)

struct daemon {
	struct vdisplay *vd;
	struct encoder *enc;
	struct vsend *vs;
	unsigned width, height;
	bool streaming;
	bool force_idr;
	uint64_t refresh_due_us;   /* re-encode the current screen at this time if nothing new arrived (0 = none) */
	uint64_t last_encode_us;
	uint64_t frames_encoded, frames_skipped, idr_frames, submit_failures, send_errors, sent_packets;
	uint64_t t_service_us, t_send_us, t_poll_us; /* where the loop spends its time */
	double encode_ms_sum, encode_ms_max;
};

static struct daemon D;

static void cb_invalidate(uint32_t lost, void *user)
{
	(void)user;
	if (D.enc) encoder_invalidate_frame(D.enc, lost);
	if (!D.refresh_due_us) D.refresh_due_us = net_now_us() + 20000; /* on a static screen nothing else would repair it */
}

static void cb_need_idr(void *user)
{
	(void)user;
	D.force_idr = true;
	if (!D.refresh_due_us) D.refresh_due_us = net_now_us() + 5000;
}

/* Encodes one screen image and hands it to the sender. */
static void encode_and_submit(const uint8_t *pixels, int stride, uint64_t capture_us)
{
	uint64_t now = net_now_us();
	if (vsend_queue_delay_us(D.vs) > 20000) { /* the previous frame is still being sent: skip this capture */
		D.frames_skipped++;
		return;
	}
	encoder_set_bitrate(D.enc, vsend_bitrate_kbps(D.vs));
	if (vsend_must_idr(D.vs, now)) { /* the newest frames may all depend on a frame the phone never got */
		D.force_idr = true;
		LOG("%u frames unresolved, rtt %.1f ms: sending an IDR", vsend_unresolved(D.vs), vsend_rtt_us(D.vs) / 1000.0);
	}
	struct encoder_packet p;
	int rc = encoder_encode(D.enc, pixels, stride, D.force_idr, &p);
	if (rc != 0) { LOG("encode failed: %s", strerror(-rc)); return; }
	D.force_idr = false;
	D.frames_encoded++;
	D.encode_ms_sum += p.upload_ms + p.convert_ms + p.encode_ms;
	if (p.upload_ms + p.convert_ms + p.encode_ms > D.encode_ms_max) D.encode_ms_max = p.upload_ms + p.convert_ms + p.encode_ms;
	if (p.idr) D.idr_frames++;
	uint32_t id = (uint32_t)p.frame_index;
	uint32_t ref = p.idr ? PROTO_NO_REF : id - (uint32_t)p.ref_distance;
	rc = vsend_submit(D.vs, id, ref, capture_us, p.idr ? PROTO_VFLAG_IDR : 0, p.data, (uint32_t)p.size, now);
	if (rc != 0) { /* an encoded frame that cannot be sent is a lost frame: keep it out of the reference chain */
		D.submit_failures++;
		encoder_invalidate_frame(D.enc, p.frame_index);
	}
	D.last_encode_us = now;
	D.refresh_due_us = 0;
}

static void on_frame(const struct vdisplay_frame *f, void *user)
{
	(void)user;
	if (!D.streaming) return;
	if ((unsigned)f->width != D.width || (unsigned)f->height != D.height) return;
	encode_and_submit(f->data, f->stride, f->grab_ns / 1000);
}

static void usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s [options]\n"
		"  -p PORT     TCP and UDP port (default 47110)\n"
		"  -n NAME     monitor name, up to 13 characters (default pdisp)\n"
		"  -t TEXT     serial text, up to 13 characters (default PDISP0001)\n"
		"  -N SERIAL   binary EDID serial (default 1)\n"
		"  -m WxH@HZ   use this mode instead of the phone's native size\n"
		"  -R MAXHZ    highest refresh rate to pick from the phone's list (default 60)\n"
		"  -b KBPS     initial bitrate (default 20000)\n"
		"  -B KBPS     highest bitrate (default 40000)\n"
		"  -r NODE     VAAPI render node (default: first that can encode HEVC)\n"
		"  -d CARD     evdi card (default: first free)\n"
		"  -S SECS     stop streaming after this long (default: until the phone leaves)\n",
		argv0);
}

static int reject(struct net_ctl *c, uint16_t reason, const char *text)
{
	struct proto_reject r;
	memset(&r, 0, sizeof r);
	r.reason = reason;
	snprintf(r.text, sizeof r.text, "%s", text);
	uint8_t b[256];
	int n = proto_reject_encode(&r, b, sizeof b);
	if (n > 0) net_ctl_send(c, PROTO_CTL_REJECT, b, (size_t)n);
	LOG("rejected the phone: %s", text);
	return 1;
}

int main(int argc, char **argv)
{
	unsigned port = 47110;
	const char *name = "pdisp", *serial_text = "PDISP0001", *render_node = NULL;
	uint32_t serial = 1;
	unsigned max_hz_mhz = 60000, bitrate = 20000, max_bitrate = 40000;
	unsigned force_w = 0, force_h = 0, force_mhz = 0;
	int card = -1, opt;
	double run_secs = 0;

	while ((opt = getopt(argc, argv, "p:n:t:N:m:R:b:B:r:d:S:h")) != -1) {
		switch (opt) {
		case 'p': port = (unsigned)atoi(optarg); break;
		case 'n': name = optarg; break;
		case 't': serial_text = optarg; break;
		case 'N': serial = (uint32_t)strtoul(optarg, NULL, 0); break;
		case 'm': {
			double hz;
			if (sscanf(optarg, "%ux%u@%lf", &force_w, &force_h, &hz) != 3) { usage(argv[0]); return 2; }
			force_mhz = (unsigned)(hz * 1000 + 0.5);
			break;
		}
		case 'R': max_hz_mhz = (unsigned)(atof(optarg) * 1000); break;
		case 'b': bitrate = (unsigned)atoi(optarg); break;
		case 'B': max_bitrate = (unsigned)atoi(optarg); break;
		case 'r': render_node = optarg; break;
		case 'd': card = atoi(optarg); break;
		case 'S': run_secs = atof(optarg); break;
		default: usage(argv[0]); return 2;
		}
	}
	struct sigaction sa = { .sa_handler = on_signal };
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);
	t_start = net_now_us();

	/* ---- wait for a phone and read its HELLO */
	int lfd = net_tcp_listen(port);
	if (lfd < 0) { fprintf(stderr, "cannot listen on %u: %s\n", port, strerror(-lfd)); return 1; }
	LOG("listening on port %u", port);
	struct pollfd lp = { .fd = lfd, .events = POLLIN };
	while (!stop && poll(&lp, 1, 200) <= 0) {}
	if (stop) return 0;
	int cfd = accept(lfd, NULL, NULL);
	close(lfd);
	if (cfd < 0) { perror("accept"); return 1; }
	net_tune_tcp(cfd);
	struct net_ctl ctl;
	net_ctl_init(&ctl, cfd);

	uint16_t type;
	const uint8_t *payload;
	size_t plen;
	struct proto_hello hello;
	if (net_ctl_recv(&ctl, &type, &payload, &plen, 10000) != 1 || type != PROTO_CTL_HELLO ||
	    proto_hello_decode(payload, plen, &hello) != 0) {
		LOG("no valid HELLO from the phone");
		return 1;
	}
	LOG("phone '%s': %ux%u at %u dpi, %u refresh rates", hello.device_name, hello.native_width, hello.native_height,
	    hello.density_dpi, hello.n_refresh);
	if (hello.proto_min > PROTO_VERSION || hello.proto_max < PROTO_VERSION)
		return reject(&ctl, 1, "no common protocol version");
	int has_hevc = 0;
	for (int i = 0; i < hello.n_codecs; i++) has_hevc |= hello.codecs[i] == PROTO_CODEC_HEVC;
	if (!has_hevc) return reject(&ctl, 4, "the phone cannot decode HEVC");

	/* ---- the mode: the phone's native size unless forced */
	unsigned w = force_w ? force_w : hello.native_width, h = force_h ? force_h : hello.native_height;
	unsigned refresh_mhz = force_mhz;
	if (!refresh_mhz) {
		for (int i = 0; i < hello.n_refresh; i++)
			if (hello.refresh_mhz[i] <= max_hz_mhz && hello.refresh_mhz[i] > refresh_mhz) refresh_mhz = hello.refresh_mhz[i];
		if (!refresh_mhz) refresh_mhz = hello.n_refresh ? hello.refresh_mhz[0] : 60000;
	}
	unsigned dpi = hello.density_dpi ? hello.density_dpi : 160;
	struct edid_params ep;
	memset(&ep, 0, sizeof ep);
	ep.width = w; ep.height = h; ep.refresh_mhz = refresh_mhz;
	ep.mm_width = (unsigned)(w * 25.4 / dpi + 0.5); ep.mm_height = (unsigned)(h * 25.4 / dpi + 0.5);
	memcpy(ep.pnp_id, "PDI", 4);
	ep.product_code = 1; ep.serial = serial; ep.year = 2026;
	snprintf(ep.name, sizeof ep.name, "%s", name);
	snprintf(ep.serial_text, sizeof ep.serial_text, "%s", serial_text);
	uint8_t edid[EDID_SIZE];
	struct edid_timing timing;
	if (w % 8 || h % 8) return reject(&ctl, 4, "width and height must be multiples of 8");
	if (edid_generate(&ep, edid) != 0 || edid_compute_timing(w, h, refresh_mhz, &timing) != 0)
		return reject(&ctl, 4, "this mode cannot be represented in an EDID");

	/* ---- the virtual output and the encoder */
	if (card < 0) card = vdisplay_find_free_device();
	if (card < 0) { LOG("no free evdi device"); reject(&ctl, 3, "no virtual display available"); return 3; }
	struct vdisplay_events ev = { .frame = on_frame };
	int err = vdisplay_open(card, &ev, &D.vd);
	if (err) { LOG("cannot open evdi card %d: %s", card, strerror(-err)); return 1; }
	if (vdisplay_connect(D.vd, edid, timing.hactive, timing.vactive, timing.pixel_clock_khz) != 0) return 1;
	D.width = w; D.height = h;
	LOG("virtual output %ux%u@%.3f created on card%d", w, h, timing.refresh_hz, card);

	struct encoder_config ec = { .render_node = render_node, .width = w, .height = h,
				     .fps = (unsigned)(timing.refresh_hz + 0.5), .bitrate_kbps = bitrate, .vbr = true,
				     .target_percent = 50, .vbv_ms = 30 };
	err = encoder_open(&ec, &D.enc);
	if (err) { LOG("cannot open the encoder: %s", strerror(-err)); reject(&ctl, 4, "no HEVC encoder"); return 1; }
	LOG("encoder: %s", encoder_description(D.enc));

	/* ---- SETUP */
	uint32_t session_id = (uint32_t)(net_now_us() * 2654435761u) ^ (uint32_t)getpid();
	int ufd = net_udp_bind(port);
	if (ufd < 0) { LOG("cannot bind UDP port %u: %s", port, strerror(-ufd)); return 1; }

	struct proto_setup su;
	memset(&su, 0, sizeof su);
	su.proto_version = PROTO_VERSION;
	su.session_id = session_id;
	su.udp_port = (uint16_t)port;
	su.params = (struct proto_stream_params){ .epoch = 0, .security = PROTO_SECURITY_NONE, .width = (uint16_t)w, .height = (uint16_t)h,
						  .refresh_mhz = refresh_mhz, .codec = PROTO_CODEC_HEVC, .color_range = 0,
						  .color_primaries = 1, .color_transfer = 13, .color_matrix = 1, .shard_size = 1200,
						  .fec_percent = 10, .bitrate_kbps = bitrate, .min_bitrate_kbps = 2000,
						  .max_bitrate_kbps = max_bitrate, .dpb_frames = 7 };
	su.params.param_sets_len = (uint16_t)encoder_parameter_sets(D.enc, su.params.param_sets, sizeof su.params.param_sets);
	uint8_t sb[1400];
	int sn = proto_setup_encode(&su, sb, sizeof sb);
	if (sn < 0 || net_ctl_send(&ctl, PROTO_CTL_SETUP, sb, (size_t)sn) != 0) { LOG("cannot send SETUP"); return 1; }

	struct vsend_config vc = { .session_id = session_id, .epoch = 0, .shard_size = 1200, .bitrate_kbps = bitrate,
				   .min_bitrate_kbps = 2000, .max_bitrate_kbps = max_bitrate };
	struct vsend_callbacks vcb = { .invalidate = cb_invalidate, .need_idr = cb_need_idr };
	D.vs = vsend_create(&vc, &vcb);
	if (!D.vs) return 1;

	/* ---- wait for READY (TCP) and the phone's first UDP packet */
	struct sockaddr_in peer;
	bool have_peer = false, ready = false;
	uint64_t deadline = net_now_us() + 15000000;
	while (!stop && !(ready && have_peer) && net_now_us() < deadline) {
		struct pollfd p[2] = { { .fd = cfd, .events = POLLIN }, { .fd = ufd, .events = POLLIN } };
		if (poll(p, 2, 100) <= 0) continue;
		if (p[1].revents & POLLIN) {
			uint8_t b[2048];
			struct sockaddr_in from;
			socklen_t fl = sizeof from;
			ssize_t n = recvfrom(ufd, b, sizeof b, 0, (struct sockaddr *)&from, &fl);
			struct proto_pkt_hdr h;
			if (n >= PROTO_PKT_HDR_SIZE && proto_pkt_hdr_read(b, (size_t)n, &h) == 0 && h.session_id == session_id &&
			    h.type == PROTO_PKT_HELLO_UDP) {
				peer = from;
				have_peer = true;
			}
		}
		if (p[0].revents & POLLIN) {
			int r = net_ctl_recv(&ctl, &type, &payload, &plen, 0);
			if (r == 1 && type == PROTO_CTL_READY) ready = true;
			else if (r < 0) { LOG("phone went away during setup"); return 1; }
		}
	}
	if (!(ready && have_peer)) { LOG("the phone did not finish setting up"); return 1; }
	char addr[64];
	inet_ntop(AF_INET, &peer.sin_addr, addr, sizeof addr);
	LOG("streaming to %s:%u (session %08x)", addr, ntohs(peer.sin_port), session_id);

	/* ---- stream: the first frame is the current screen, so a static desktop shows up at once */
	D.streaming = true;
	struct vdisplay_frame cur;
	if (vdisplay_current_frame(D.vd, &cur)) encode_and_submit(cur.data, cur.stride, net_now_us());
	else D.refresh_due_us = net_now_us() + 100000;

	uint64_t last_rx = net_now_us(), end_us = run_secs > 0 ? net_now_us() + (uint64_t)(run_secs * 1e6) : 0;
	uint64_t last_report = net_now_us();
	int evfd = vdisplay_event_fd(D.vd);
	uint8_t pkt[2048];
	while (!stop) {
		uint64_t now = net_now_us();
		if (end_us && now >= end_us) break;
		uint64_t due = vsend_next_due_us(D.vs, now);
		int timeout_ms = due <= now ? 0 : (int)((due - now + 999) / 1000);
		if (timeout_ms > 10) timeout_ms = 10;
		struct pollfd p[3] = { { .fd = evfd, .events = POLLIN }, { .fd = ufd, .events = POLLIN }, { .fd = cfd, .events = POLLIN } };
		uint64_t t_a = net_now_us();
		poll(p, 3, timeout_ms);
		now = net_now_us();
		D.t_poll_us += now - t_a;

		vdisplay_service(D.vd);
		D.t_service_us += net_now_us() - now;

		for (;;) { /* feedback and other packets from the phone */
			struct sockaddr_in from;
			socklen_t fl = sizeof from;
			ssize_t n = recvfrom(ufd, pkt, sizeof pkt, 0, (struct sockaddr *)&from, &fl);
			if (n < 0) break;
			if (from.sin_addr.s_addr != peer.sin_addr.s_addr || from.sin_port != peer.sin_port) continue;
			last_rx = now;
			vsend_on_packet(D.vs, pkt, (size_t)n, now);
		}
		if (p[2].revents & (POLLIN | POLLHUP)) {
			int r = net_ctl_recv(&ctl, &type, &payload, &plen, 0);
			if (r == 1) {
				if (type == PROTO_CTL_CLOSE) { LOG("the phone closed the session"); break; }
				if (type == PROTO_CTL_PING) net_ctl_send(&ctl, PROTO_CTL_PONG, payload, plen);
			} else if (r < 0) { LOG("control connection closed"); break; }
		}
		if (now - last_rx > 3000000) { LOG("no packets from the phone for 3 s"); break; }

		if (D.refresh_due_us && now >= D.refresh_due_us && vdisplay_current_frame(D.vd, &cur)) {
			D.refresh_due_us = 0;
			encode_and_submit(cur.data, cur.stride, now); /* same screen again, predicted from a frame the phone has */
		}

		for (int guard = 0; guard < 256; guard++) { /* send what is due */
			now = net_now_us();
			if (vsend_next_due_us(D.vs, now) > now) break;
			int n = vsend_next_packet(D.vs, now, pkt, sizeof pkt);
			if (n <= 0) break;
			if (sendto(ufd, pkt, (size_t)n, 0, (struct sockaddr *)&peer, sizeof peer) < 0) D.send_errors++;
			else D.sent_packets++;
		}

		if (now - last_report >= 2000000) {
			last_report = now;
			const struct vsend_stats *st = vsend_stats(D.vs);
			LOG("loop: poll %.0f%% service(capture+encode) %.0f%%, sent %llu packets, %llu send errors, queue %.1f ms, unresolved %u",
			    D.t_poll_us / 20000.0, D.t_service_us / 20000.0, (unsigned long long)D.sent_packets,
			    (unsigned long long)D.send_errors, vsend_queue_delay_us(D.vs) / 1000.0, vsend_unresolved(D.vs));
			D.t_poll_us = D.t_service_us = 0;
			LOG("frames %llu (skipped %llu, idr %llu), %.1f Mbit/s target, FEC %u%%, repairs %llu, nacks %llu, lost %llu",
			    (unsigned long long)D.frames_encoded, (unsigned long long)D.frames_skipped, (unsigned long long)D.idr_frames,
			    vsend_bitrate_kbps(D.vs) / 1000.0, vsend_fec_percent(D.vs), (unsigned long long)st->repair_packets,
			    (unsigned long long)st->nacks, (unsigned long long)st->frames_lost);
		}
	}

	const struct vsend_stats *st = vsend_stats(D.vs);
	printf("DAEMON frames=%llu skipped=%llu idr=%llu packets=%llu repair_packets=%llu nacks=%llu frames_lost=%llu "
	       "encode_ms_avg=%.2f encode_ms_max=%.2f\n",
	       (unsigned long long)D.frames_encoded, (unsigned long long)D.frames_skipped, (unsigned long long)D.idr_frames,
	       (unsigned long long)st->packets, (unsigned long long)st->repair_packets, (unsigned long long)st->nacks,
	       (unsigned long long)st->frames_lost, D.frames_encoded ? D.encode_ms_sum / D.frames_encoded : 0.0, D.encode_ms_max);
	uint8_t cb[64];
	struct proto_close cl;
	memset(&cl, 0, sizeof cl);
	int cn = proto_close_encode(&cl, cb, sizeof cb);
	if (cn > 0) net_ctl_send(&ctl, PROTO_CTL_CLOSE, cb, (size_t)cn);
	vsend_destroy(D.vs);
	encoder_close(D.enc);
	vdisplay_close(D.vd);
	net_ctl_close(&ctl);
	close(ufd);
	return 0;
}
