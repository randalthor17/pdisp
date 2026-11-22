/* pdispd: the pdisp daemon. Accepts one phone on TCP, creates a virtual monitor sized for it, then captures the monitor,
 * encodes it and streams it over UDP with loss repair (docs/protocol.md). When the phone reports a new display size
 * (rotation) the output, encoder and sender are rebuilt in a new epoch and the phone is told with RECONFIGURE.
 *
 *   pdispd [-p PORT] [-n NAME] [-t SERIAL] [-m WxH@HZ] [-R MAXHZ] [-b KBPS] [-B MAXKBPS] [-F REFINE_FRAMES] [-r NODE] [-d CARD] [-S SECS]
 *
 * Exit: 0 after the phone left or -S elapsed, 1 on error, 3 if there is no free evdi device.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <sys/wait.h>
#include <time.h>

#include "edid.h"
#include "encoder.h"
#include "handshake.h"
#include "identity.h"
#include "inputseq.h"
#include "net.h"
#include "proto.h"
#include "touch.h"
#include "usblink.h"
#include "usbmux.h"
#include "vdisplay.h"
#include "video.h"
#include "vsend.h"

static volatile sig_atomic_t stop;
static void on_signal(int sig) { (void)sig; stop = 1; }

static uint64_t t_start;
#define LOG(...) do { fprintf(stderr, "[%8.3f] ", (net_now_us() - t_start) / 1e6); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)

/* ---- configuration from the command line */

static struct {
	unsigned port, max_hz_mhz, bitrate, max_bitrate, force_w, force_h, force_mhz;
	const char *name, *serial_text, *render_node;
	uint32_t serial;
	int card;
	const char *config_dir;  /* identity and authorized_keys live here (default ~/.config/pdisp) */
	int pair_mode;           /* what to do with a device that is not in authorized_keys */
	bool insecure;           /* development only: no handshake, no encryption (the M4 behaviour) */
	bool no_touch;           /* do not create a touchscreen for the phone */
	const char *touch_hook;  /* a command run when the touchscreen exists, to bind it to the virtual output (docs/touch.md) */
	unsigned grace_secs;     /* after a lost link, keep the virtual output this long for the phone to come back (0 = tear it down at once) */
	bool once;               /* serve one session and exit instead of waiting for the next device */
	bool usb;          /* talk to a tablet over a USB cable (accessory mode) instead of listening on the network */
	unsigned usb_vid;  /* only consider this USB vendor (0 = any) */
	unsigned refine_frames, refine_min_qp, refine_start_qp, refine_rate_percent, refine_gap_ms, refine_settle_ms; /* static refinement (see D.refine_left); 0 frames = off */
} C = { .port = 47110, .max_hz_mhz = 60000, .bitrate = 12000, .max_bitrate = 40000, .name = "pdisp",
	.serial_text = "PDISP0001", .serial = 1, .card = -1, .grace_secs = 30 };

/* ---- the live session */

struct daemon {
	struct vdisplay *vd;
	struct encoder *enc;
	struct vsend *vs;
	unsigned width, height, refresh_mhz;
	uint32_t session_id;
	uint8_t epoch;
	bool streaming;
	bool awaiting_ack;         /* RECONFIGURE sent, the phone has not confirmed yet */
	uint64_t ack_deadline_us;
	bool new_epoch_pending;    /* the next frame is the first of a new epoch */
	bool force_idr;
	uint64_t refresh_due_us;   /* re-encode the current screen at this time if nothing new arrived (0 = none) */
	uint64_t refine_due_us;    /* when the next refinement of a static screen is due (0 = none) */
	uint64_t dump_due_us;      /* debug (PDISP_DUMP_FRAME): when to write the captured screen to a file, a second after it last changed */
	const char *dump_path;
	unsigned refine_left;      /* refinement pictures still to send after the last change of the screen */
	uint64_t frames_encoded, frames_skipped, idr_frames, submit_failures, send_errors, sent_packets, reconfigs, pongs;
	double encode_ms_sum, encode_ms_max;
	uint64_t t_service_us, t_poll_us, auth_drops;
	int card_index; /* the evdi card in use, for finding the connector name */
};

static struct daemon D;

enum { PAIR_ASK, PAIR_ACCEPT, PAIR_DENY };
static struct touchdev *touchdev;  /* the phone's touchscreen on this PC, while a session is running */
static struct pendev *pendev;      /* its stylus, if it has one */
static struct input_rx inrx;
static unsigned long touch_events_applied;
static struct sec_udp secudp; /* datagram keys of the current session (valid when !C.insecure) */

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
/* Debug aid for quality checks: the screen exactly as it was captured (what the encoder was given), as a PPM. */
static void dump_frame(const uint8_t *pixels, int stride, unsigned w, unsigned h, const char *path)
{
	FILE *fp = fopen(path, "wb");
	if (!fp) return;
	fprintf(fp, "P6\n%u %u\n255\n", w, h);
	uint8_t *row = malloc((size_t)w * 3);
	for (unsigned y = 0; row && y < h; y++) {
		const uint8_t *s = pixels + (size_t)y * (size_t)stride;
		for (unsigned x = 0; x < w; x++) { row[x * 3] = s[x * 4 + 2]; row[x * 3 + 1] = s[x * 4 + 1]; row[x * 3 + 2] = s[x * 4]; } /* XRGB8888, little endian: B G R X */
		fwrite(row, 1, (size_t)w * 3, fp);
	}
	free(row);
	fclose(fp);
}

enum pic_kind { PIC_NEW, PIC_REPAIR, PIC_REFINE }; /* a new capture; the same screen again to repair a loss or a skip; a refinement of a static screen */

static void encode_and_submit(const uint8_t *pixels, int stride, uint64_t capture_us, enum pic_kind kind)
{
	uint64_t now = net_now_us();
	if (vsend_queue_delay_us(D.vs) > 20000) { /* the previous frame is still being sent: skip this capture */
		D.frames_skipped++;
		/* The screen may not change again, and then this was its last image: look at the current screen again soon, once the
		 * queue has drained. Without this a static screen stays stale on the phone until something else repaints it. */
		if (kind == PIC_REFINE) D.refine_due_us = now + 10000;
		else if (!D.refresh_due_us) D.refresh_due_us = now + 10000;
		return;
	}
	encoder_set_refine(D.enc, kind == PIC_REFINE, C.refine_min_qp, C.refine_start_qp, C.refine_rate_percent);
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
	double ms = p.upload_ms + p.convert_ms + p.encode_ms;
	D.encode_ms_sum += ms;
	if (ms > D.encode_ms_max) D.encode_ms_max = ms;
	if (p.idr) D.idr_frames++;
	uint32_t id = (uint32_t)p.frame_index;
	uint32_t ref = p.idr ? PROTO_NO_REF : id - (uint32_t)p.ref_distance;
	uint8_t vflags = p.idr ? PROTO_VFLAG_IDR : 0;
	if (D.new_epoch_pending) { vflags |= PROTO_VFLAG_NEW_EPOCH; D.new_epoch_pending = false; }
	rc = vsend_submit(D.vs, id, ref, capture_us, vflags, p.data, (uint32_t)p.size, now);
	if (rc != 0) { /* an encoded frame that cannot be sent is a lost frame: keep it out of the reference chain */
		D.submit_failures++;
		encoder_invalidate_frame(D.enc, p.frame_index);
	}
	D.refresh_due_us = 0;
	if (kind != PIC_REFINE && D.dump_path) D.dump_due_us = now + 1000000;
	if (kind == PIC_NEW) { /* the screen changed: if it stops changing, improve the picture a few times */
		D.refine_left = C.refine_frames;
		D.refine_due_us = C.refine_frames ? now + C.refine_settle_ms * 1000ull : 0; /* only once the screen has really settled */
	} else if (kind == PIC_REFINE) {
		D.refine_due_us = --D.refine_left ? now + C.refine_gap_ms * 1000ull : 0;
	}
}

static void on_frame(const struct vdisplay_frame *f, void *user)
{
	(void)user;
	if (!D.streaming) return;
	if ((unsigned)f->width != D.width || (unsigned)f->height != D.height) return;
	encode_and_submit(f->data, f->stride, f->grab_ns / 1000, PIC_NEW);
}

/* ---- choosing a mode and bringing the pipeline up */

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

/* The mode for a phone's display fields: its native size unless forced, the best refresh rate within the cap. Returns 0, or
 * a REJECT reason (with `*why`). */
static int choose_mode(const struct proto_hello *h, struct edid_params *ep, struct edid_timing *t, uint8_t edid[EDID_SIZE],
		       const char **why)
{
	unsigned w = C.force_w ? C.force_w : h->native_width, hh = C.force_h ? C.force_h : h->native_height;
	unsigned refresh_mhz = C.force_mhz;
	if (!refresh_mhz) {
		for (int i = 0; i < h->n_refresh; i++)
			if (h->refresh_mhz[i] <= C.max_hz_mhz && h->refresh_mhz[i] > refresh_mhz) refresh_mhz = h->refresh_mhz[i];
		if (!refresh_mhz) refresh_mhz = h->n_refresh ? h->refresh_mhz[0] : 60000;
	}
	unsigned dpi = h->density_dpi ? h->density_dpi : 160;
	memset(ep, 0, sizeof *ep);
	ep->width = w; ep->height = hh; ep->refresh_mhz = refresh_mhz;
	ep->mm_width = (unsigned)(w * 25.4 / dpi + 0.5);
	ep->mm_height = (unsigned)(hh * 25.4 / dpi + 0.5);
	memcpy(ep->pnp_id, "PDI", 4);
	ep->product_code = 1; ep->serial = C.serial; ep->year = 2026;
	snprintf(ep->name, sizeof ep->name, "%s", C.name);
	snprintf(ep->serial_text, sizeof ep->serial_text, "%s", C.serial_text);
	if (w % 8 || hh % 8) { *why = "width and height must be multiples of 8"; return 4; }
	if (edid_generate(ep, edid) != 0 || edid_compute_timing(w, hh, refresh_mhz, t) != 0) {
		*why = "this mode cannot be represented in an EDID";
		return 4;
	}
	return 0;
}

/* Brings up (or re-brings up) the virtual output and the encoder for a mode. Returns 0, or a negative errno. */
static int bring_up(const struct edid_params *ep, const struct edid_timing *t, const uint8_t edid[EDID_SIZE])
{
	if (!D.vd) {
		int card = C.card >= 0 ? C.card : vdisplay_find_free_device();
		if (card < 0) return -ENODEV;
		struct vdisplay_events ev = { .frame = on_frame };
		int err = vdisplay_open(card, &ev, &D.vd);
		if (err) return err;
		LOG("using evdi card%d", card);
		D.card_index = card;
	} else {
		vdisplay_disconnect(D.vd);
	}
	int err = vdisplay_connect(D.vd, edid, t->hactive, t->vactive, t->pixel_clock_khz);
	if (err) return err;
	D.width = ep->width; D.height = ep->height; D.refresh_mhz = ep->refresh_mhz;
	LOG("virtual output %ux%u@%.3f", D.width, D.height, t->refresh_hz);

	if (D.enc) { encoder_close(D.enc); D.enc = NULL; }
	struct encoder_config ec = { .render_node = C.render_node, .width = D.width, .height = D.height,
				     .fps = (unsigned)(t->refresh_hz + 0.5), .bitrate_kbps = C.bitrate, .vbr = true,
				     .target_percent = 50, .vbv_ms = 30 };
	err = encoder_open(&ec, &D.enc);
	if (err) return err;
	LOG("encoder: %s", encoder_description(D.enc));
	return 0;
}

static void make_params(struct proto_stream_params *p)
{
	memset(p, 0, sizeof *p);
	p->epoch = D.epoch;
	p->security = C.insecure ? PROTO_SECURITY_NONE : PROTO_SECURITY_CHACHA20POLY1305;
	p->width = (uint16_t)D.width;
	p->height = (uint16_t)D.height;
	p->refresh_mhz = D.refresh_mhz;
	p->codec = PROTO_CODEC_HEVC;
	p->color_range = 0; p->color_primaries = 1; p->color_transfer = 13; p->color_matrix = 1;
	p->shard_size = 1200;
	p->fec_percent = 10;
	p->bitrate_kbps = C.bitrate;
	p->min_bitrate_kbps = 2000;
	p->max_bitrate_kbps = C.max_bitrate;
	p->dpb_frames = 7;
	p->param_sets_len = (uint16_t)encoder_parameter_sets(D.enc, p->param_sets, sizeof p->param_sets);
}

static int make_sender(void)
{
	if (D.vs) vsend_destroy(D.vs);
	struct vsend_config vc = { .session_id = D.session_id, .epoch = D.epoch, .shard_size = 1200, .bitrate_kbps = C.bitrate,
				   .min_bitrate_kbps = 2000, .max_bitrate_kbps = C.max_bitrate,
				   .frame_interval_us = D.refresh_mhz ? 1000000000ull / D.refresh_mhz : 0 };
	struct vsend_callbacks vcb = { .invalidate = cb_invalidate, .need_idr = cb_need_idr };
	D.vs = vsend_create(&vc, &vcb);
	return D.vs ? 0 : -ENOMEM;
}

/* a datagram to the phone: sendto() on the network, plain send() on the connected USB socket */
static ssize_t udp_send(int fd, const void *buf, size_t n, const struct sockaddr_in *peer)
{
	uint8_t sealed[2048 + SEC_UDP_OVERHEAD];
	if (!C.insecure) {
		if (n > 2048) return -1;
		memcpy(sealed, buf, n);
		int sn = sec_udp_seal(&secudp, sealed, n, sizeof sealed);
		if (sn < 0) return -1;
		buf = sealed;
		n = (size_t)sn;
	}
	if (C.usb) return send(fd, buf, n, MSG_DONTWAIT);
	return sendto(fd, buf, n, 0, (const struct sockaddr *)peer, sizeof *peer);
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
		"  -b KBPS     initial bitrate (default 12000; the controller raises it while the link allows)\n"
		"  -B KBPS     highest bitrate (default 40000)\n"
		"  -r NODE     VAAPI render node (default: first that can encode HEVC)\n"
		"  -d CARD     evdi card (default: first free)\n"
		"  -S SECS     stop streaming after this long (default: until the phone leaves)\n"
		"  -u          serve a tablet over USB (AOA accessory mode) instead of the network; keeps waiting for the next\n"
		"              plug-in after each session unless -S is given\n"
		"  -V VID      with -u, only consider this USB vendor id in hex (e.g. 04e8)\n"
		"  -K DIR      identity and authorized_keys directory (default ~/.config/pdisp)\n"
		"  -P MODE     a device that is not authorised yet: ask (prompt on stdin, the default), accept (add it: development)\n"
		"              or deny\n"
		"  -T          do not create a touchscreen for the phone's touches\n"
		"  -H CMD      run this shell command once the touchscreen exists, with PDISP_OUTPUT (connector, if known), PDISP_OUTPUT_ID\n"
		"              (\"make model serial\" as the compositor names it), PDISP_TOUCH_DEVICE, PDISP_WIDTH, PDISP_HEIGHT set: use it\n"
		"              to bind the touchscreen to the virtual output (see docs/touch.md)\n"
		"  -I          INSECURE: no handshake and no encryption; for development only\n"
		"  -g SECS     when the link to the phone is lost (cable pulled, Wi-Fi gone, phone asleep), keep the virtual monitor this long for\n"
		"              it to come back, so windows stay where they are; then remove it (default 30, 0 = remove it at once). A phone that\n"
		"              disconnects on purpose or locks its screen is removed at once.\n"
		"  -o          serve one session and exit (default: keep waiting for the next device)\n",
		argv0);
}

static int run_session(double run_secs);

int main(int argc, char **argv)
{
	int opt;
	double run_secs = 0;
	{ /* static refinement settings; environment variables while the defaults are being worked out */
		const char *e;
		/* Static refinement (docs/quality.md): 40 pictures of the same screen, each a little better, once it has not changed for 150 ms. */
		C.refine_frames = (e = getenv("PDISP_REFINE_FRAMES")) ? (unsigned)atoi(e) : 40;
		C.refine_min_qp = (e = getenv("PDISP_REFINE_MINQP")) ? (unsigned)atoi(e) : 4;
		C.refine_gap_ms = (e = getenv("PDISP_REFINE_GAP_MS")) ? (unsigned)atoi(e) : 50;
		C.refine_settle_ms = (e = getenv("PDISP_REFINE_SETTLE_MS")) ? (unsigned)atoi(e) : 150;
		C.refine_start_qp = (e = getenv("PDISP_REFINE_QP0")) ? (unsigned)atoi(e) : 22;
		C.refine_rate_percent = (e = getenv("PDISP_REFINE_RATE_PCT")) ? (unsigned)atoi(e) : 50;
	}
	while ((opt = getopt(argc, argv, "p:n:t:N:m:R:b:B:r:d:S:g:uV:K:P:H:F:TIho")) != -1) {
		switch (opt) {
		case 'p': C.port = (unsigned)atoi(optarg); break;
		case 'F': C.refine_frames = (unsigned)atoi(optarg); break; /* static refinement pictures; 0 = off */
		case 'n': C.name = optarg; break;
		case 't': C.serial_text = optarg; break;
		case 'N': C.serial = (uint32_t)strtoul(optarg, NULL, 0); break;
		case 'm': {
			double hz;
			if (sscanf(optarg, "%ux%u@%lf", &C.force_w, &C.force_h, &hz) != 3) { usage(argv[0]); return 2; }
			C.force_mhz = (unsigned)(hz * 1000 + 0.5);
			break;
		}
		case 'R': C.max_hz_mhz = (unsigned)(atof(optarg) * 1000); break;
		case 'b': C.bitrate = (unsigned)atoi(optarg); break;
		case 'B': C.max_bitrate = (unsigned)atoi(optarg); break;
		case 'r': C.render_node = optarg; break;
		case 'd': C.card = atoi(optarg); break;
		case 'S': run_secs = atof(optarg); break;
		case 'u': C.usb = true; break;
		case 'K': C.config_dir = optarg; break;
		case 'P':
			if (!strcmp(optarg, "ask")) C.pair_mode = PAIR_ASK;
			else if (!strcmp(optarg, "accept")) C.pair_mode = PAIR_ACCEPT;
			else if (!strcmp(optarg, "deny")) C.pair_mode = PAIR_DENY;
			else { usage(argv[0]); return 2; }
			break;
		case 'I': C.insecure = true; break;
		case 'T': C.no_touch = true; break;
		case 'H': C.touch_hook = optarg; break;
		case 'o': C.once = true; break;
		case 'g': C.grace_secs = (unsigned)atoi(optarg); break;
		case 'V': C.usb_vid = (unsigned)strtoul(optarg, NULL, 16); break;
		default: usage(argv[0]); return 2;
		}
	}
	struct sigaction sa = { .sa_handler = on_signal };
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);
	t_start = net_now_us();

	if ((C.usb || !C.once) && run_secs <= 0) {
		/* one child per plug-in: a session leaves the evdi device, encoder and sockets in a state that is simplest to throw away */
		while (!stop) {
			pid_t pid = fork();
			if (pid < 0) { perror("fork"); return 1; }
			if (pid == 0) return run_session(0);
			int status = 0;
			while (waitpid(pid, &status, 0) < 0 && errno == EINTR && !stop) {}
			if (stop) { kill(pid, SIGTERM); waitpid(pid, &status, 0); break; }
			if (WIFEXITED(status) && WEXITSTATUS(status) == 3) return 3; /* no evdi device: waiting will not help */
			struct timespec nap = { 1, 0 };
			nanosleep(&nap, NULL);
		}
		return 0;
	}
	return run_session(run_secs);
}

/* The compositor's name for the virtual output (its DRM connector, e.g. DVI-I-1), from sysfs, or "" if it is not there (yet). */
static void connector_name(char *out, size_t cap)
{
	out[0] = 0;
	if (D.card_index < 0) return;
	char prefix[32], path[320];
	snprintf(prefix, sizeof prefix, "card%d-", D.card_index);
	DIR *d = opendir("/sys/class/drm");
	if (!d) return;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (strncmp(e->d_name, prefix, strlen(prefix)) != 0) continue;
		snprintf(path, sizeof path, "/sys/class/drm/%s/status", e->d_name);
		FILE *f = fopen(path, "r");
		char st[32] = "";
		if (f) { if (!fgets(st, sizeof st, f)) st[0] = 0; fclose(f); }
		if (strncmp(st, "connected", 9) == 0) { snprintf(out, cap, "%s", e->d_name + strlen(prefix)); break; }
	}
	closedir(d);
}

/* (Re)creates the phone's touchscreen for the current virtual output size, and tells the hook (if any). */
static struct { unsigned w, h, dpi, slots, caps; } touch_sig; /* what the current devices were made for */

static void touch_stop(void)
{
	if (touchdev) { touch_close(touchdev); touchdev = NULL; }
	if (pendev) { pen_close(pendev); pendev = NULL; }
	memset(&touch_sig, 0, sizeof touch_sig);
}

static void touch_start(const struct proto_hello *h, unsigned w, unsigned hgt, const struct edid_params *ep)
{
	unsigned slots = h->max_touch_points ? (h->max_touch_points > TOUCH_MAX_SLOTS ? TOUCH_MAX_SLOTS : h->max_touch_points) : 1;
	if ((touchdev || pendev) && touch_sig.w == w && touch_sig.h == hgt && touch_sig.dpi == h->density_dpi && touch_sig.slots == slots &&
	    touch_sig.caps == (h->device_caps & (PROTO_CAP_TOUCH | PROTO_CAP_STYLUS))) {
		/* the same phone (or one just like it) came back: the compositor keeps its binding of these devices, so they stay */
		if (touchdev) touch_release_all(touchdev);
		if (pendev) pen_release_all(pendev);
		input_rx_init(&inrx);
		return;
	}
	touch_stop();
	touch_sig.w = w; touch_sig.h = hgt; touch_sig.dpi = h->density_dpi; touch_sig.slots = slots;
	touch_sig.caps = h->device_caps & (PROTO_CAP_TOUCH | PROTO_CAP_STYLUS);
	input_rx_init(&inrx);
	if (C.no_touch || C.insecure || !(h->device_caps & (PROTO_CAP_TOUCH | PROTO_CAP_STYLUS))) {
		LOG("no touchscreen: %s", C.no_touch ? "disabled with -T" : C.insecure ? "not without security (-I)" : "the phone has none");
		return;
	}
	struct touch_config tc = { .name = "pdisp touchscreen", .width = w, .height = hgt, .dpi = h->density_dpi,
				   .slots = h->max_touch_points ? (h->max_touch_points > TOUCH_MAX_SLOTS ? TOUCH_MAX_SLOTS : h->max_touch_points) : 1 };
	int e = 0;
	if (h->device_caps & PROTO_CAP_TOUCH) {
		e = touch_open(&tc, &touchdev);
		if (e) { LOG("cannot create the touchscreen: %s (touch needs write access to /dev/uinput)", strerror(-e)); touchdev = NULL; return; }
	}
	struct pen_config pc = { .name = "pdisp stylus", .width = w, .height = hgt, .dpi = h->density_dpi };
	if (h->device_caps & PROTO_CAP_STYLUS) {
		e = pen_open(&pc, &pendev);
		if (e) { LOG("cannot create the pen tablet: %s", strerror(-e)); pendev = NULL; }
	}
	char out[64], id[160];
	connector_name(out, sizeof out);
	snprintf(id, sizeof id, "PNP(%s) %s %s", ep->pnp_id, ep->name, ep->serial_text);
	LOG("input devices created: %ux%u, %s%s%s, output '%s' (%s)", w, hgt, touchdev ? "touchscreen" : "", touchdev && pendev ? " and " : "",
	    pendev ? "pen tablet" : "", out[0] ? out : "unknown", id);
	if (C.touch_hook) {
		signal(SIGCHLD, SIG_IGN); /* nobody waits for the hook */
		pid_t pid = fork();
		if (pid == 0) {
			char wb[16], hb[16];
			snprintf(wb, sizeof wb, "%u", w);
			snprintf(hb, sizeof hb, "%u", hgt);
			setenv("PDISP_OUTPUT", out, 1);
			setenv("PDISP_OUTPUT_ID", id, 1);
			setenv("PDISP_TOUCH_DEVICE", touchdev ? tc.name : "", 1);
			setenv("PDISP_STYLUS_DEVICE", pendev ? pc.name : "", 1);
			setenv("PDISP_WIDTH", wb, 1);
			setenv("PDISP_HEIGHT", hb, 1);
			execl("/bin/sh", "sh", "-c", C.touch_hook, (char *)NULL);
			_exit(127);
		}
	} else {
		LOG("the compositor must map these devices to the virtual output. niri: input { %s%s%s } (see docs/touch.md)",
		    touchdev ? "touch { map-to-output \"" : "", touchdev ? id : "", touchdev ? "\" } " : "");
		if (pendev) LOG("... and for the stylus: input { tablet { map-to-output \"%s\" } }", id);
	}
}

/* An INPUT datagram from the phone: apply each new event once, in order, then acknowledge. */
static void handle_input(const uint8_t *payload, size_t len, int ufd, const struct sockaddr_in *peer)
{
	struct proto_input in;
	if (proto_input_decode(payload, len, &in) != 0) return;
	struct proto_input_event ev[PROTO_MAX_INPUT_EVENTS];
	int gap;
	unsigned n = input_rx_accept(&inrx, &in, ev, &gap);
	if (gap) {
		LOG("input events were lost for good: lifting every finger and the pen");
		if (touchdev) touch_release_all(touchdev);
		if (pendev) pen_release_all(pendev);
	}
	for (unsigned i = 0; i < n; i++) { /* each device takes the events of its own tool and ignores the rest */
		if (touchdev) touch_event(touchdev, &ev[i]);
		if (pendev) pen_event(pendev, &ev[i]);
		touch_events_applied++;
	}
	uint8_t body[4], pkt[PROTO_PKT_HDR_SIZE + 4];
	if (proto_u32_encode(input_rx_acked(&inrx), body, sizeof body) == 4) {
		int pn = vsend_make_packet(D.vs, PROTO_PKT_INPUT_ACK, body, 4, pkt, sizeof pkt);
		if (pn > 0) udp_send(ufd, pkt, (size_t)pn, peer);
	}
}

/* Waits for one line on stdin (the pairing prompt's answer). Returns its first character lowercased, 0 on end of input or error,
 * or -1 on timeout. */
static int read_answer(int timeout_ms)
{
	struct pollfd p = { .fd = 0, .events = POLLIN };
	int c = 0, first = 1;
	for (;;) {
		int r = poll(&p, 1, timeout_ms);
		if (r == 0) return -1;
		if (r < 0) { if (errno == EINTR && !stop) continue; return 0; }
		char ch;
		ssize_t n = read(0, &ch, 1);
		if (n <= 0) return first ? 0 : c;
		if (ch == '\n') return c ? c : 'n';
		if (first && ch != ' ') { c = (ch >= 'A' && ch <= 'Z') ? ch - 'A' + 'a' : ch; first = 0; }
	}
}

/* Decides whether a device whose key the handshake proved may use this PC. Returns 1 to continue; otherwise the device has been
 * told why and the caller ends the session. */
static int authorize(struct net_ctl *ctl, const struct hs_peer *peer, const char *ks_path)
{
	char fp[IDENTITY_FINGERPRINT_LEN], known[64];
	identity_fingerprint(peer->pub, fp);
	int k = keystore_find(ks_path, peer->pub, known, sizeof known);
	if (k > 0) { LOG("authorised device '%s' (key %s)", known, fp); return 1; }
	if (k < 0) { LOG("cannot read %s: %s", ks_path, strerror(-k)); reject(ctl, 2, "this PC cannot read its list of authorised devices"); return 0; }
	LOG("unknown device '%s' (key %s), pairing code %06u", peer->name, fp, peer->sas);
	if (C.pair_mode == PAIR_DENY) { reject(ctl, 2, "this device is not authorised"); return 0; }
	if (C.pair_mode == PAIR_ACCEPT) {
		LOG("WARNING: -P accept: authorising '%s' without asking", peer->name);
		int e = keystore_add(ks_path, peer->pub, peer->name);
		if (e) LOG("could not save the key: %s", strerror(-e));
		return 1;
	}
	net_ctl_send(ctl, PROTO_CTL_PAIRING_REQUIRED, NULL, 0); /* the device shows its own copy of the code */
	fprintf(stderr, "\nA device wants to use this PC as a second monitor.\n   name: %s\n   key:  %s\n   code: %06u   (it must be the same on the device's screen)\n"
			"Allow it?  [y] always  [o] this time only  [N] no: ", peer->name, fp, peer->sas);
	fflush(stderr);
	int a = read_answer(90000);
	if (a == 'y' || a == 'o') {
		fprintf(stderr, "\n");
		if (a == 'y') {
			int e = keystore_add(ks_path, peer->pub, peer->name);
			if (e) LOG("could not save the key: %s", strerror(-e));
			else LOG("authorised '%s' (key %s) for good", peer->name, fp);
		} else {
			LOG("authorised '%s' (key %s) for this session only", peer->name, fp);
		}
		return 1;
	}
	fprintf(stderr, "\n");
	if (a == -1) { LOG("no answer: pairing with '%s' timed out", peer->name); reject(ctl, 6, "pairing timed out on the PC"); }
	else { LOG("pairing with '%s' was denied", peer->name); reject(ctl, 5, "pairing was denied on the PC"); }
	return 0;
}

/* ================= sessions and the virtual output's life =================
 *
 * The virtual monitor belongs to the daemon, not to one connection. A phone that disconnects on purpose or locks its screen takes the
 * monitor away at once; a phone that is merely lost (cable pulled, Wi-Fi gone, asleep) leaves it in place for a grace period, so the
 * windows on it stay put and the same phone can pick up where it left off. If nobody comes back the monitor is removed and the compositor
 * moves its windows to the other outputs. A phone coming back to a monitor of the right size reuses it without any change visible to the
 * compositor; one that needs another size gets the monitor reconfigured in place.
 */

enum end_reason {
	END_STOP,     /* the daemon is shutting down */
	END_USER,     /* the phone disconnected on purpose */
	END_LOCK,     /* the phone's screen locked */
	END_LINK,     /* the link failed: the phone may come back */
	END_REJECTED, /* nobody was served (refused, failed setup): the monitor, if there is one, is as it was */
	END_FATAL,    /* the daemon cannot go on */
};

struct conn {
	int cfd, ufd;        /* the control stream and the datagram socket (over USB, the multiplexer's ends) */
	struct usblink *link;
	struct usbmux *mux;
	struct net_ctl ctl;
	struct hs_peer dev;  /* the authenticated device */
	struct sockaddr_in peer;
};

static struct identity g_id;
static char g_ks_path[1100];
static int g_lfd = -1;
static int g_exit_code;

static void conn_init(struct conn *c)
{
	memset(c, 0, sizeof *c);
	c->cfd = c->ufd = -1;
	c->ctl.fd = -1;
}

static void conn_close(struct conn *c)
{
	net_ctl_close(&c->ctl); /* closes cfd */
	if (c->ufd >= 0) close(c->ufd);
	if (c->mux) usbmux_stop(c->mux);
	if (c->link) usblink_close(c->link);
	conn_init(c);
}

/* Removes the virtual monitor: the compositor moves its windows to the other outputs. */
static void teardown_output(void)
{
	if (!D.vd && !D.enc) return;
	LOG("removing the virtual output");
	touch_stop();
	if (D.vs) { vsend_destroy(D.vs); D.vs = NULL; }
	if (D.enc) { encoder_close(D.enc); D.enc = NULL; }
	if (D.vd) { vdisplay_close(D.vd); D.vd = NULL; }
	D.streaming = false;
	D.width = D.height = D.refresh_mhz = 0;
}

/* Keeps capturing while nobody is connected, so the compositor's updates to the virtual output never back up. */
static void service_display(void)
{
	if (D.vd) vdisplay_service(D.vd);
}

/* Waits for a phone to connect and pass the secure handshake. `deadline_us` is when to give up (0 = never). Returns 0, -ETIMEDOUT (the
 * grace period ran out), -ECANCELED (shutting down) or another negative errno (fatal). */
static int acquire_peer(struct conn *c, uint64_t deadline_us)
{
	for (;;) {
		conn_init(c);
		if (C.usb) {
			for (;;) { /* in half-second slices, so the display keeps being serviced and the deadline is honoured */
				if (stop) return -ECANCELED;
				if (deadline_us && net_now_us() >= deadline_us) return -ETIMEDOUT;
				service_display();
				int rd, wr;
				int e = usblink_open(500, C.usb_vid, (const volatile int *)&stop, &c->link, &rd, &wr);
				if (e == -ETIMEDOUT) continue;
				if (e == -ECANCELED) return e;
				if (e) { LOG("cannot open the USB device: %s (is the udev rule installed? see contrib/udev)", strerror(-e)); return e; }
				LOG("USB accessory %s", usblink_description(c->link));
				int usb_dgram;
				e = usbmux_start(rd, wr, 0, &c->mux, &c->cfd, &usb_dgram);
				if (e) { LOG("cannot start the USB multiplexer: %s", strerror(-e)); usblink_close(c->link); c->link = NULL; return e; }
				fcntl(usb_dgram, F_SETFL, fcntl(usb_dgram, F_GETFL) | O_NONBLOCK);
				c->ufd = usb_dgram;
				break;
			}
		} else {
			for (;;) {
				if (stop) return -ECANCELED;
				if (deadline_us && net_now_us() >= deadline_us) return -ETIMEDOUT;
				struct pollfd lp[2] = { { .fd = g_lfd, .events = POLLIN }, { .fd = D.vd ? vdisplay_event_fd(D.vd) : -1, .events = POLLIN } };
				poll(lp, 2, 200);
				service_display();
				if (lp[0].revents & POLLIN) {
					c->cfd = accept(g_lfd, NULL, NULL);
					if (c->cfd < 0) { if (errno == EINTR || errno == EAGAIN) continue; perror("accept"); return -errno; }
					net_tune_tcp(c->cfd);
					break;
				}
			}
		}
		net_ctl_init(&c->ctl, c->cfd);
		if (C.insecure) return 0;
		int r = hs_respond(&c->ctl, &g_id, C.name, 10000, &c->dev, &secudp);
		if (r == 0) return 0;
		LOG("a peer failed the handshake (%s): dropped", strerror(-r));
		conn_close(c);
	}
}

/* One phone, from its HELLO to the end of the stream. The virtual output and encoder outlive this call (see above). */
static enum end_reason serve(struct conn *c, double run_secs)
{
	struct net_ctl *ctl = &c->ctl;
	uint16_t type;
	const uint8_t *payload;
	size_t plen;
	struct proto_hello hello;
	if (net_ctl_recv(ctl, &type, &payload, &plen, C.usb ? 120000 : 10000) != 1 || type != PROTO_CTL_HELLO ||
	    proto_hello_decode(payload, plen, &hello) != 0) {
		LOG("no valid HELLO from the phone");
		return END_REJECTED;
	}
	LOG("phone '%s': %ux%u at %u dpi, %u refresh rates", hello.device_name, hello.native_width, hello.native_height,
	    hello.density_dpi, hello.n_refresh);
	if (!C.insecure && !authorize(ctl, &c->dev, g_ks_path)) return END_REJECTED;
	if (hello.proto_min > PROTO_VERSION || hello.proto_max < PROTO_VERSION) { reject(ctl, 1, "no common protocol version"); return END_REJECTED; }
	int has_hevc = 0;
	for (int i = 0; i < hello.n_codecs; i++) has_hevc |= hello.codecs[i] == PROTO_CODEC_HEVC;
	if (!has_hevc) { reject(ctl, 4, "the phone cannot decode HEVC"); return END_REJECTED; }

	struct edid_params ep;
	struct edid_timing timing;
	uint8_t edid[EDID_SIZE];
	const char *why = "";
	int reason = choose_mode(&hello, &ep, &timing, edid, &why);
	if (reason) { reject(ctl, (uint16_t)reason, why); return END_REJECTED; }

	/* the monitor: still there from before (a phone that came back in time), or new */
	if (D.vd && D.enc && D.width == ep.width && D.height == ep.height && D.refresh_mhz == ep.refresh_mhz) {
		LOG("the virtual output %ux%u is still there: reusing it", D.width, D.height);
	} else {
		int err = bring_up(&ep, &timing, edid);
		if (err == -ENODEV && !D.vd) { LOG("no free evdi device"); reject(ctl, 3, "no virtual display available"); g_exit_code = 3; return END_FATAL; }
		if (err) { LOG("cannot bring up the output and encoder: %s", strerror(-err)); reject(ctl, 4, "no HEVC encoder or display"); return END_REJECTED; }
	}
	touch_start(&hello, D.width, D.height, &ep);
	D.epoch = 0;
	D.awaiting_ack = false;
	D.new_epoch_pending = false;
	D.force_idr = true; /* the stream starts from a keyframe, whatever the encoder sent before */
	D.refresh_due_us = 0;
	D.refine_due_us = 0;
	D.refine_left = 0;
	D.dump_due_us = 0;
	D.dump_path = getenv("PDISP_DUMP_FRAME");

	/* ---- SETUP */
	D.session_id = (uint32_t)(net_now_us() * 2654435761u) ^ (uint32_t)getpid();
	int ufd = c->ufd;
	if (!C.usb) {
		ufd = net_udp_bind(C.port);
		if (ufd < 0) { LOG("cannot bind UDP port %u: %s", C.port, strerror(-ufd)); return END_REJECTED; }
		c->ufd = ufd;
	}
	struct proto_setup su;
	memset(&su, 0, sizeof su);
	su.proto_version = PROTO_VERSION;
	su.session_id = D.session_id;
	su.udp_port = (uint16_t)C.port;
	make_params(&su.params);
	uint8_t sb[1400];
	int sn = proto_setup_encode(&su, sb, sizeof sb);
	if (sn < 0 || net_ctl_send(ctl, PROTO_CTL_SETUP, sb, (size_t)sn) != 0) { LOG("cannot send SETUP"); return END_LINK; }
	if (make_sender() != 0) return END_FATAL;

	/* ---- wait for READY (TCP) and the phone's first UDP packet */
	struct sockaddr_in *peer = &c->peer;
	bool have_peer = C.usb, ready = false; /* over USB the datagram socket is already connected */
	uint64_t deadline = net_now_us() + 15000000;
	while (!stop && !(ready && have_peer) && net_now_us() < deadline) {
		struct pollfd p[2] = { { .fd = c->cfd, .events = POLLIN }, { .fd = ufd, .events = POLLIN } };
		if (poll(p, 2, 100) <= 0) { service_display(); continue; }
		if (p[1].revents & POLLIN) {
			uint8_t b[2048];
			struct sockaddr_in from;
			socklen_t fl = sizeof from;
			ssize_t n = recvfrom(ufd, b, sizeof b, 0, (struct sockaddr *)&from, &fl);
			if (n > 0 && !C.insecure) { int o = sec_udp_open(&secudp, b, (size_t)n); n = o < 0 ? -1 : o; D.auth_drops += o < 0; }
			struct proto_pkt_hdr h;
			if (n >= PROTO_PKT_HDR_SIZE && proto_pkt_hdr_read(b, (size_t)n, &h) == 0 && h.session_id == D.session_id &&
			    h.type == PROTO_PKT_HELLO_UDP) {
				*peer = from;
				have_peer = true;
			}
		}
		if (p[0].revents & POLLIN) {
			int r = net_ctl_recv(ctl, &type, &payload, &plen, 0);
			if (r == 1 && type == PROTO_CTL_READY) ready = true;
			else if (r < 0) { LOG("phone went away during setup"); return END_LINK; }
		}
	}
	if (stop) return END_STOP;
	if (!(ready && have_peer)) { LOG("the phone did not finish setting up"); return END_LINK; }
	char addr[64];
	inet_ntop(AF_INET, &peer->sin_addr, addr, sizeof addr);
	LOG("streaming to %s:%u (session %08x)", addr, ntohs(peer->sin_port), D.session_id);

	/* ---- stream: the first frame is the current screen, so a static desktop shows up at once */
	D.streaming = true;
	struct vdisplay_frame cur;
	if (vdisplay_current_frame(D.vd, &cur)) encode_and_submit(cur.data, cur.stride, net_now_us(), PIC_NEW);
	else D.refresh_due_us = net_now_us() + 100000;

	enum end_reason end = END_LINK;
	uint64_t last_rx = net_now_us(), end_us = run_secs > 0 ? net_now_us() + (uint64_t)(run_secs * 1e6) : 0;
	uint64_t last_report = net_now_us(), next_ping = net_now_us() + 2000000, last_pong = net_now_us();
	int evfd = vdisplay_event_fd(D.vd);
	uint8_t pkt[2048];
	int done = 0;
	while (!done) {
		if (stop) { end = END_STOP; break; }
		uint64_t now = net_now_us();
		if (end_us && now >= end_us) { end = END_STOP; break; }
		uint64_t due = vsend_next_due_us(D.vs, now);
		int timeout_ms = due <= now ? 0 : (int)((due - now + 999) / 1000);
		if (timeout_ms > 10) timeout_ms = 10;
		struct pollfd p[3] = { { .fd = evfd, .events = POLLIN }, { .fd = ufd, .events = POLLIN }, { .fd = c->cfd, .events = POLLIN } };
		uint64_t t_a = net_now_us();
		poll(p, 3, timeout_ms);
		now = net_now_us();
		D.t_poll_us += now - t_a;

		vdisplay_service(D.vd);
		D.t_service_us += net_now_us() - now;

		for (;;) { /* feedback, time requests and other packets from the phone */
			struct sockaddr_in from;
			socklen_t fl = sizeof from;
			ssize_t n = recvfrom(ufd, pkt, sizeof pkt, 0, (struct sockaddr *)&from, &fl);
			if (n < 0) break;
			if (!C.insecure) { /* anything that does not authenticate (or is a replay) never reaches the protocol code */
				int o = sec_udp_open(&secudp, pkt, (size_t)n);
				if (o < 0) { D.auth_drops++; continue; }
				n = o;
			}
			if (!C.usb && (from.sin_addr.s_addr != peer->sin_addr.s_addr || from.sin_port != peer->sin_port)) continue;
			last_rx = now;
			struct proto_pkt_hdr ph;
			if (n >= PROTO_PKT_HDR_SIZE + 8 && proto_pkt_hdr_read(pkt, (size_t)n, &ph) == 0 && ph.type == PROTO_PKT_TIME_REQ &&
			    ph.session_id == D.session_id) {
				/* clock sync: t2 is when this arrived, t3 when the answer leaves */
				struct proto_time_resp tr = { .t2 = net_now_us() };
				uint8_t rp[PROTO_PKT_HDR_SIZE + 24], body[24];
				if (proto_token_decode(pkt + PROTO_PKT_HDR_SIZE, 8, &tr.t1) == 0) {
					tr.t3 = net_now_us();
					int bn = proto_time_resp_encode(&tr, body, sizeof body);
					int rn = bn > 0 ? vsend_make_packet(D.vs, PROTO_PKT_TIME_RESP, body, (size_t)bn, rp, sizeof rp) : -1;
					if (rn > 0) udp_send(ufd, rp, (size_t)rn, peer);
				}
				continue;
			}
			struct proto_pkt_hdr ih;
			if (proto_pkt_hdr_read(pkt, (size_t)n, &ih) == 0 && ih.type == PROTO_PKT_INPUT && ih.session_id == D.session_id) {
				handle_input(pkt + PROTO_PKT_HDR_SIZE, (size_t)n - PROTO_PKT_HDR_SIZE, ufd, peer);
				continue;
			}
			vsend_on_packet(D.vs, pkt, (size_t)n, now);
		}

		/* the control channel */
		if (p[2].revents & (POLLIN | POLLHUP)) {
			for (;;) {
				int r = net_ctl_recv(ctl, &type, &payload, &plen, 0);
				if (r == 0) break;
				if (r < 0) { LOG("control connection closed"); end = END_LINK; done = 1; break; }
				if (type == PROTO_CTL_CLOSE) {
					struct proto_close cl;
					uint16_t why_closed = proto_close_decode(payload, plen, &cl) == 0 ? cl.reason : 0;
					LOG("the phone closed the session (%s)", why_closed == 1 ? "its screen locked" : why_closed == 3 ? "error" : "on purpose");
					end = why_closed == 1 ? END_LOCK : why_closed == 3 ? END_LINK : END_USER;
					done = 1;
					break;
				}
				if (type == PROTO_CTL_PING) net_ctl_send(ctl, PROTO_CTL_PONG, payload, plen);
				else if (type == PROTO_CTL_PONG) { D.pongs++; last_pong = now; }
				else if (type == PROTO_CTL_RECONFIGURE_ACK && D.awaiting_ack) {
					D.awaiting_ack = false;
					D.streaming = true;
					D.new_epoch_pending = true;
					D.force_idr = true;
					LOG("the phone confirmed epoch %u: streaming %ux%u", D.epoch, D.width, D.height);
					if (vdisplay_current_frame(D.vd, &cur) && (unsigned)cur.width == D.width && (unsigned)cur.height == D.height)
						encode_and_submit(cur.data, cur.stride, net_now_us(), PIC_NEW);
				} else if (type == PROTO_CTL_DISPLAY_CHANGED) {
					struct proto_hello nh;
					struct edid_params nep;
					struct edid_timing nt;
					uint8_t nedid[EDID_SIZE];
					const char *nwhy = "";
					if (proto_hello_decode(payload, plen, &nh) != 0 || choose_mode(&nh, &nep, &nt, nedid, &nwhy) != 0) {
						LOG("ignoring a display change that cannot be honoured: %s", nwhy);
					} else if (nep.width == D.width && nep.height == D.height && nep.refresh_mhz == D.refresh_mhz) {
						LOG("display change without a mode change: nothing to do");
					} else {
						LOG("the phone's display changed to %ux%u: reconfiguring", nep.width, nep.height);
						D.streaming = false;
						D.refresh_due_us = 0;
						D.refine_due_us = 0;
						if (bring_up(&nep, &nt, nedid) != 0 || ++D.epoch == 0) { LOG("reconfiguration failed"); end = END_LINK; done = 1; break; }
						touch_start(&nh, D.width, D.height, &nep); /* the coordinate range changed with the size */
						if (make_sender() != 0) { end = END_FATAL; done = 1; break; }
						if (!C.insecure) sec_udp_set_epoch(&secudp, D.epoch); /* sequence numbers restart: new datagram keys */
						struct proto_stream_params rp;
						make_params(&rp);
						uint8_t rb[1400];
						int rn = proto_stream_params_encode(&rp, rb, sizeof rb);
						if (rn < 0 || net_ctl_send(ctl, PROTO_CTL_RECONFIGURE, rb, (size_t)rn) != 0) { end = END_LINK; done = 1; break; }
						D.awaiting_ack = true;
						D.ack_deadline_us = net_now_us() + 10000000;
						D.reconfigs++;
						evfd = vdisplay_event_fd(D.vd);
					}
				}
			}
			if (done) break;
		}
		if (D.awaiting_ack && now > D.ack_deadline_us) { LOG("the phone did not confirm the reconfiguration"); end = END_LINK; break; }
		if (now > next_ping) { /* liveness of the control channel itself */
			next_ping = now + 2000000;
			uint8_t tb[8];
			if (proto_token_encode(now, tb, sizeof tb) == 8) net_ctl_send(ctl, PROTO_CTL_PING, tb, 8);
		}
		if (now - last_pong > 8000000 && D.pongs) { LOG("no PONG for 8 s: the control channel is dead"); end = END_LINK; break; }
		if (C.usb && usblink_gone(c->link)) { LOG("the USB cable was unplugged"); end = END_LINK; break; }
		if (now - last_rx > 3000000 && !D.awaiting_ack) { LOG("no packets from the phone for 3 s"); end = END_LINK; break; }

		if (D.refresh_due_us && now >= D.refresh_due_us && D.streaming && vdisplay_current_frame(D.vd, &cur) &&
		    (unsigned)cur.width == D.width && (unsigned)cur.height == D.height) {
			D.refresh_due_us = 0;
			encode_and_submit(cur.data, cur.stride, now, PIC_REPAIR); /* same screen again, predicted from a frame the phone has */
		}
		if (D.dump_due_us && now >= D.dump_due_us && D.streaming && vdisplay_current_frame(D.vd, &cur) &&
		    (unsigned)cur.width == D.width && (unsigned)cur.height == D.height) {
			D.dump_due_us = 0;
			dump_frame(cur.data, cur.stride, D.width, D.height, D.dump_path);
		}
		if (D.refine_due_us && now >= D.refine_due_us && (vsend_queue_delay_us(D.vs) > 20000 || vsend_bitrate_kbps(D.vs) < 12000))
			D.refine_due_us = now + C.refine_gap_ms * 1000ull; /* the link is busy or slow: refinement frames would only queue up, get lost and keep the rate down. Wait. */
		if (D.refine_due_us && now >= D.refine_due_us && D.streaming && vdisplay_current_frame(D.vd, &cur) &&
		    (unsigned)cur.width == D.width && (unsigned)cur.height == D.height) {
			D.refine_due_us = 0;
			encode_and_submit(cur.data, cur.stride, now, PIC_REFINE); /* the screen stopped changing: better quality for it */
		}

		for (int guard = 0; guard < 256; guard++) { /* send what is due */
			now = net_now_us();
			if (vsend_next_due_us(D.vs, now) > now) break;
			int n = vsend_next_packet(D.vs, now, pkt, sizeof pkt);
			if (n <= 0) break;
			if (udp_send(ufd, pkt, (size_t)n, peer) < 0) D.send_errors++;
			else D.sent_packets++;
		}

		if (now - last_report >= 2000000) {
			last_report = now;
			const struct vsend_stats *st = vsend_stats(D.vs);
			LOG("frames %llu (skipped %llu, idr %llu), %.1f Mbit/s target, FEC %u%%, repairs %llu, nacks %llu, lost %llu, "
			    "queue %.1f ms, unresolved %u, phone: queue %.1f ms decode %.1f ms recv >= %.1f Mbit/s",
			    (unsigned long long)D.frames_encoded, (unsigned long long)D.frames_skipped, (unsigned long long)D.idr_frames,
			    vsend_bitrate_kbps(D.vs) / 1000.0, vsend_fec_percent(D.vs), (unsigned long long)st->repair_packets,
			    (unsigned long long)st->nacks, (unsigned long long)st->frames_lost, vsend_queue_delay_us(D.vs) / 1000.0,
			    vsend_unresolved(D.vs), st->fb_queue_max_us / 1000.0, st->fb_decode_max_x10 / 10.0, st->fb_recv_kbps_min / 1000.0);
			vsend_reset_report(D.vs);
		}
	}

	const struct vsend_stats *st = vsend_stats(D.vs);
	printf("DAEMON frames=%llu skipped=%llu idr=%llu packets=%llu repair_packets=%llu nacks=%llu frames_lost=%llu "
	       "encode_ms_avg=%.2f encode_ms_max=%.2f reconfigs=%llu pongs=%llu auth_drops=%llu touch_events=%lu\n",
	       (unsigned long long)D.frames_encoded, (unsigned long long)D.frames_skipped, (unsigned long long)D.idr_frames,
	       (unsigned long long)st->packets, (unsigned long long)st->repair_packets, (unsigned long long)st->nacks,
	       (unsigned long long)st->frames_lost, D.frames_encoded ? D.encode_ms_sum / D.frames_encoded : 0.0, D.encode_ms_max,
	       (unsigned long long)D.reconfigs, (unsigned long long)D.pongs, (unsigned long long)D.auth_drops, touch_events_applied);
	fflush(stdout);
	if (end == END_STOP) { /* tell the phone we are going away on purpose */
		uint8_t cb[64];
		struct proto_close cl;
		memset(&cl, 0, sizeof cl);
		int cn = proto_close_encode(&cl, cb, sizeof cb);
		if (cn > 0) net_ctl_send(ctl, PROTO_CTL_CLOSE, cb, (size_t)cn);
	}
	/* the session is over: no more video, fingers lifted; the monitor and encoder stay for the caller to decide about */
	D.streaming = false;
	if (D.vs) { vsend_destroy(D.vs); D.vs = NULL; }
	if (touchdev) touch_release_all(touchdev);
	if (pendev) pen_release_all(pendev);
	return end;
}

static int run_session(double run_secs)
{
	t_start = net_now_us();
	if (C.insecure) {
		LOG("WARNING: running without authentication or encryption (-I): development only");
	} else {
		char cdir[900];
		if (C.config_dir) snprintf(cdir, sizeof cdir, "%s", C.config_dir);
		else if (identity_default_dir(cdir, sizeof cdir) != 0) { LOG("cannot work out the config directory: use -K DIR"); return 1; }
		int created, e = identity_load_or_create(cdir, &g_id, &created);
		if (e) { LOG("cannot use the identity in %s: %s", cdir, strerror(-e)); return 1; }
		char fp[IDENTITY_FINGERPRINT_LEN];
		identity_fingerprint(g_id.pub, fp);
		LOG("%s identity in %s, key %s", created ? "created a new" : "using the", cdir, fp);
		snprintf(g_ks_path, sizeof g_ks_path, "%s/authorized_keys", cdir);
	}
	if (!C.usb) {
		g_lfd = net_tcp_listen(C.port);
		if (g_lfd < 0) { fprintf(stderr, "cannot listen on %u: %s\n", C.port, strerror(-g_lfd)); return 1; }
		LOG("listening on port %u", C.port);
	} else {
		LOG("waiting for a tablet on USB");
	}

	bool single = C.once || run_secs > 0; /* -o and -S serve one session and exit */
	uint64_t grace_deadline = 0; /* while the monitor waits for a lost phone: when it gives up */
	int exit_code = 0;
	for (;;) {
		struct conn c;
		int r = acquire_peer(&c, grace_deadline);
		if (r == -ECANCELED) break;
		if (r == -ETIMEDOUT) {
			LOG("the phone did not come back within %u s", C.grace_secs);
			teardown_output();
			grace_deadline = 0;
			if (single) break;
			continue;
		}
		if (r) { exit_code = 1; break; }
		enum end_reason end = serve(&c, run_secs);
		conn_close(&c);
		if (end == END_FATAL) { exit_code = g_exit_code ? g_exit_code : 1; break; }
		if (end == END_STOP) break;
		if (end == END_REJECTED) { if (single) break; continue; } /* the monitor and any grace period are as they were */
		if (single) break;
		if (end == END_LINK && C.grace_secs && D.vd) {
			grace_deadline = net_now_us() + (uint64_t)C.grace_secs * 1000000;
			LOG("lost the phone: keeping the virtual output for %u s in case it comes back", C.grace_secs);
		} else {
			teardown_output(); /* on purpose, or its screen locked, or no grace period: the compositor moves the windows away now */
			grace_deadline = 0;
		}
	}
	teardown_output();
	if (g_lfd >= 0) close(g_lfd);
	return exit_code;
}
