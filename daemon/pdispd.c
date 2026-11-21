/* pdispd: the pdisp daemon. Accepts one phone on TCP, creates a virtual monitor sized for it, then captures the monitor,
 * encodes it and streams it over UDP with loss repair (docs/protocol.md). When the phone reports a new display size
 * (rotation) the output, encoder and sender are rebuilt in a new epoch and the phone is told with RECONFIGURE.
 *
 *   pdispd [-p PORT] [-n NAME] [-t SERIAL] [-m WxH@HZ] [-R MAXHZ] [-b KBPS] [-B MAXKBPS] [-r NODE] [-d CARD] [-S SECS]
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
	bool once;               /* serve one session and exit instead of waiting for the next device */
	bool usb;          /* talk to a tablet over a USB cable (accessory mode) instead of listening on the network */
	unsigned usb_vid;  /* only consider this USB vendor (0 = any) */
} C = { .port = 47110, .max_hz_mhz = 60000, .bitrate = 12000, .max_bitrate = 40000, .name = "pdisp",
	.serial_text = "PDISP0001", .serial = 1, .card = -1 };

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
}

static void on_frame(const struct vdisplay_frame *f, void *user)
{
	(void)user;
	if (!D.streaming) return;
	if ((unsigned)f->width != D.width || (unsigned)f->height != D.height) return;
	encode_and_submit(f->data, f->stride, f->grab_ns / 1000);
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
				   .min_bitrate_kbps = 2000, .max_bitrate_kbps = C.max_bitrate };
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
		"  -o          serve one session and exit (default: keep waiting for the next device)\n",
		argv0);
}

static int run_session(double run_secs);

int main(int argc, char **argv)
{
	int opt;
	double run_secs = 0;
	while ((opt = getopt(argc, argv, "p:n:t:N:m:R:b:B:r:d:S:uV:K:P:H:TIho")) != -1) {
		switch (opt) {
		case 'p': C.port = (unsigned)atoi(optarg); break;
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
static void touch_start(const struct proto_hello *h, unsigned w, unsigned hgt, const struct edid_params *ep)
{
	if (touchdev) { touch_close(touchdev); touchdev = NULL; }
	if (pendev) { pen_close(pendev); pendev = NULL; }
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

/* One session: wait for the tablet (network or cable), set up the monitor and stream until it leaves. */
static int run_session(double run_secs)
{
	t_start = net_now_us();
	struct identity id;
	char ks_path[1100] = "";
	if (C.insecure) {
		LOG("WARNING: running without authentication or encryption (-I): development only");
	} else {
		char cdir[900];
		if (C.config_dir) snprintf(cdir, sizeof cdir, "%s", C.config_dir);
		else if (identity_default_dir(cdir, sizeof cdir) != 0) { LOG("cannot work out the config directory: use -K DIR"); return 1; }
		int created, e = identity_load_or_create(cdir, &id, &created);
		if (e) { LOG("cannot use the identity in %s: %s", cdir, strerror(-e)); return 1; }
		char fp[IDENTITY_FINGERPRINT_LEN];
		identity_fingerprint(id.pub, fp);
		LOG("%s identity in %s, key %s", created ? "created a new" : "using the", cdir, fp);
		snprintf(ks_path, sizeof ks_path, "%s/authorized_keys", cdir);
	}

	/* ---- wait for a phone that passes the handshake, then read its HELLO */
	int cfd, usb_dgram = -1, lfd = -1;
	struct usblink *link = NULL;
	struct usbmux *mux = NULL;
	struct net_ctl ctl;
	struct hs_peer dev; /* the authenticated device */
	memset(&dev, 0, sizeof dev);
	if (!C.usb) {
		lfd = net_tcp_listen(C.port);
		if (lfd < 0) { fprintf(stderr, "cannot listen on %u: %s\n", C.port, strerror(-lfd)); return 1; }
		LOG("listening on port %u", C.port);
	}
	for (;;) {
		if (C.usb) {
			LOG("waiting for a tablet on USB");
			int rd, wr;
			int e = usblink_open(-1, C.usb_vid, (const volatile int *)&stop, &link, &rd, &wr);
			if (e == -ECANCELED) return 0;
			if (e) { LOG("cannot open the USB device: %s (is the udev rule installed? see contrib/udev)", strerror(-e)); return 1; }
			LOG("USB accessory %s", usblink_description(link));
			e = usbmux_start(rd, wr, 0, &mux, &cfd, &usb_dgram);
			if (e) { LOG("cannot start the USB multiplexer: %s", strerror(-e)); return 1; }
			fcntl(usb_dgram, F_SETFL, fcntl(usb_dgram, F_GETFL) | O_NONBLOCK);
		} else {
			struct pollfd lp = { .fd = lfd, .events = POLLIN };
			while (!stop && poll(&lp, 1, 200) <= 0) {}
			if (stop) return 0;
			cfd = accept(lfd, NULL, NULL);
			if (cfd < 0) { perror("accept"); return 1; }
			net_tune_tcp(cfd);
		}
		net_ctl_init(&ctl, cfd);
		if (C.insecure) break;
		int r = hs_respond(&ctl, &id, C.name, 10000, &dev, &secudp);
		if (r == 0) break;
		LOG("a peer failed the handshake (%s): dropped", strerror(-r));
		net_ctl_close(&ctl);
		if (C.usb) return 1; /* the supervisor starts again */
	}
	if (lfd >= 0) close(lfd);

	uint16_t type;
	const uint8_t *payload;
	size_t plen;
	struct proto_hello hello;
	if (net_ctl_recv(&ctl, &type, &payload, &plen, C.usb ? 120000 : 10000) != 1 || type != PROTO_CTL_HELLO ||
	    proto_hello_decode(payload, plen, &hello) != 0) {
		LOG("no valid HELLO from the phone");
		return 1;
	}
	LOG("phone '%s': %ux%u at %u dpi, %u refresh rates", hello.device_name, hello.native_width, hello.native_height,
	    hello.density_dpi, hello.n_refresh);
	if (!C.insecure && !authorize(&ctl, &dev, ks_path)) return 1;
	if (hello.proto_min > PROTO_VERSION || hello.proto_max < PROTO_VERSION)
		return reject(&ctl, 1, "no common protocol version");
	int has_hevc = 0;
	for (int i = 0; i < hello.n_codecs; i++) has_hevc |= hello.codecs[i] == PROTO_CODEC_HEVC;
	if (!has_hevc) return reject(&ctl, 4, "the phone cannot decode HEVC");

	struct edid_params ep;
	struct edid_timing timing;
	uint8_t edid[EDID_SIZE];
	const char *why = "";
	int reason = choose_mode(&hello, &ep, &timing, edid, &why);
	if (reason) return reject(&ctl, (uint16_t)reason, why);

	int err = bring_up(&ep, &timing, edid);
	if (err == -ENODEV && !D.vd) { LOG("no free evdi device"); reject(&ctl, 3, "no virtual display available"); return 3; }
	if (err) { LOG("cannot bring up the output and encoder: %s", strerror(-err)); reject(&ctl, 4, "no HEVC encoder or display"); return 1; }
	touch_start(&hello, D.width, D.height, &ep);

	/* ---- SETUP */
	D.session_id = (uint32_t)(net_now_us() * 2654435761u) ^ (uint32_t)getpid();
	int ufd = C.usb ? usb_dgram : net_udp_bind(C.port);
	if (ufd < 0) { LOG("cannot bind UDP port %u: %s", C.port, strerror(-ufd)); return 1; }
	struct proto_setup su;
	memset(&su, 0, sizeof su);
	su.proto_version = PROTO_VERSION;
	su.session_id = D.session_id;
	su.udp_port = (uint16_t)C.port;
	make_params(&su.params);
	uint8_t sb[1400];
	int sn = proto_setup_encode(&su, sb, sizeof sb);
	if (sn < 0 || net_ctl_send(&ctl, PROTO_CTL_SETUP, sb, (size_t)sn) != 0) { LOG("cannot send SETUP"); return 1; }
	if (make_sender() != 0) return 1;

	/* ---- wait for READY (TCP) and the phone's first UDP packet */
	struct sockaddr_in peer;
	bool have_peer = C.usb, ready = false; /* over USB the datagram socket is already connected */
	uint64_t deadline = net_now_us() + 15000000;
	while (!stop && !(ready && have_peer) && net_now_us() < deadline) {
		struct pollfd p[2] = { { .fd = cfd, .events = POLLIN }, { .fd = ufd, .events = POLLIN } };
		if (poll(p, 2, 100) <= 0) continue;
		if (p[1].revents & POLLIN) {
			uint8_t b[2048];
			struct sockaddr_in from;
			socklen_t fl = sizeof from;
			ssize_t n = recvfrom(ufd, b, sizeof b, 0, (struct sockaddr *)&from, &fl);
			if (n > 0 && !C.insecure) { int o = sec_udp_open(&secudp, b, (size_t)n); n = o < 0 ? -1 : o; D.auth_drops += o < 0; }
			struct proto_pkt_hdr h;
			if (n >= PROTO_PKT_HDR_SIZE && proto_pkt_hdr_read(b, (size_t)n, &h) == 0 && h.session_id == D.session_id &&
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
	LOG("streaming to %s:%u (session %08x)", addr, ntohs(peer.sin_port), D.session_id);

	/* ---- stream: the first frame is the current screen, so a static desktop shows up at once */
	D.streaming = true;
	struct vdisplay_frame cur;
	if (vdisplay_current_frame(D.vd, &cur)) encode_and_submit(cur.data, cur.stride, net_now_us());
	else D.refresh_due_us = net_now_us() + 100000;

	uint64_t last_rx = net_now_us(), end_us = run_secs > 0 ? net_now_us() + (uint64_t)(run_secs * 1e6) : 0;
	uint64_t last_report = net_now_us(), next_ping = net_now_us() + 2000000, last_pong = net_now_us();
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
			if (!C.usb && (from.sin_addr.s_addr != peer.sin_addr.s_addr || from.sin_port != peer.sin_port)) continue;
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
					if (rn > 0) udp_send(ufd, rp, (size_t)rn, &peer);
				}
				continue;
			}
			struct proto_pkt_hdr ih;
			if (proto_pkt_hdr_read(pkt, (size_t)n, &ih) == 0 && ih.type == PROTO_PKT_INPUT && ih.session_id == D.session_id) {
				handle_input(pkt + PROTO_PKT_HDR_SIZE, (size_t)n - PROTO_PKT_HDR_SIZE, ufd, &peer);
				continue;
			}
			vsend_on_packet(D.vs, pkt, (size_t)n, now);
		}

		/* the control channel */
		if (p[2].revents & (POLLIN | POLLHUP)) {
			for (;;) {
				int r = net_ctl_recv(&ctl, &type, &payload, &plen, 0);
				if (r == 0) break;
				if (r < 0) { LOG("control connection closed"); stop = 1; break; }
				if (type == PROTO_CTL_CLOSE) { LOG("the phone closed the session"); stop = 1; break; }
				if (type == PROTO_CTL_PING) net_ctl_send(&ctl, PROTO_CTL_PONG, payload, plen);
				else if (type == PROTO_CTL_PONG) { D.pongs++; last_pong = now; }
				else if (type == PROTO_CTL_RECONFIGURE_ACK && D.awaiting_ack) {
					D.awaiting_ack = false;
					D.streaming = true;
					D.new_epoch_pending = true;
					D.force_idr = true;
					LOG("the phone confirmed epoch %u: streaming %ux%u", D.epoch, D.width, D.height);
					if (vdisplay_current_frame(D.vd, &cur) && (unsigned)cur.width == D.width && (unsigned)cur.height == D.height)
						encode_and_submit(cur.data, cur.stride, net_now_us());
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
						if (bring_up(&nep, &nt, nedid) != 0 || ++D.epoch == 0) { LOG("reconfiguration failed"); stop = 1; break; }
						touch_start(&nh, D.width, D.height, &nep); /* the coordinate range changed with the size */
						if (make_sender() != 0) { stop = 1; break; }
						if (!C.insecure) sec_udp_set_epoch(&secudp, D.epoch); /* sequence numbers restart: new datagram keys */
						struct proto_stream_params rp;
						make_params(&rp);
						uint8_t rb[1400];
						int rn = proto_stream_params_encode(&rp, rb, sizeof rb);
						if (rn < 0 || net_ctl_send(&ctl, PROTO_CTL_RECONFIGURE, rb, (size_t)rn) != 0) { stop = 1; break; }
						D.awaiting_ack = true;
						D.ack_deadline_us = net_now_us() + 10000000;
						D.reconfigs++;
						evfd = vdisplay_event_fd(D.vd);
					}
				}
			}
			if (stop) break;
		}
		if (D.awaiting_ack && now > D.ack_deadline_us) { LOG("the phone did not confirm the reconfiguration"); break; }
		if (now > next_ping) { /* liveness of the control channel itself */
			next_ping = now + 2000000;
			uint8_t tb[8];
			if (proto_token_encode(now, tb, sizeof tb) == 8) net_ctl_send(&ctl, PROTO_CTL_PING, tb, 8);
		}
		if (now - last_pong > 8000000 && D.pongs) { LOG("no PONG for 8 s: the control channel is dead"); break; }
		if (C.usb && usblink_gone(link)) { LOG("the USB cable was unplugged"); break; }
		if (now - last_rx > 3000000 && !D.awaiting_ack) { LOG("no packets from the phone for 3 s"); break; }

		if (D.refresh_due_us && now >= D.refresh_due_us && D.streaming && vdisplay_current_frame(D.vd, &cur) &&
		    (unsigned)cur.width == D.width && (unsigned)cur.height == D.height) {
			D.refresh_due_us = 0;
			encode_and_submit(cur.data, cur.stride, now); /* same screen again, predicted from a frame the phone has */
		}

		for (int guard = 0; guard < 256; guard++) { /* send what is due */
			now = net_now_us();
			if (vsend_next_due_us(D.vs, now) > now) break;
			int n = vsend_next_packet(D.vs, now, pkt, sizeof pkt);
			if (n <= 0) break;
			if (udp_send(ufd, pkt, (size_t)n, &peer) < 0) D.send_errors++;
			else D.sent_packets++;
		}

		if (now - last_report >= 2000000) {
			last_report = now;
			const struct vsend_stats *st = vsend_stats(D.vs);
			LOG("frames %llu (skipped %llu, idr %llu), %.1f Mbit/s target, FEC %u%%, repairs %llu, nacks %llu, lost %llu, "
			    "queue %.1f ms, unresolved %u",
			    (unsigned long long)D.frames_encoded, (unsigned long long)D.frames_skipped, (unsigned long long)D.idr_frames,
			    vsend_bitrate_kbps(D.vs) / 1000.0, vsend_fec_percent(D.vs), (unsigned long long)st->repair_packets,
			    (unsigned long long)st->nacks, (unsigned long long)st->frames_lost, vsend_queue_delay_us(D.vs) / 1000.0,
			    vsend_unresolved(D.vs));
		}
	}

	const struct vsend_stats *st = vsend_stats(D.vs);
	printf("DAEMON frames=%llu skipped=%llu idr=%llu packets=%llu repair_packets=%llu nacks=%llu frames_lost=%llu "
	       "encode_ms_avg=%.2f encode_ms_max=%.2f reconfigs=%llu pongs=%llu auth_drops=%llu touch_events=%lu\n",
	       (unsigned long long)D.frames_encoded, (unsigned long long)D.frames_skipped, (unsigned long long)D.idr_frames,
	       (unsigned long long)st->packets, (unsigned long long)st->repair_packets, (unsigned long long)st->nacks,
	       (unsigned long long)st->frames_lost, D.frames_encoded ? D.encode_ms_sum / D.frames_encoded : 0.0, D.encode_ms_max,
	       (unsigned long long)D.reconfigs, (unsigned long long)D.pongs, (unsigned long long)D.auth_drops, touch_events_applied);
	uint8_t cb[64];
	struct proto_close cl;
	memset(&cl, 0, sizeof cl);
	int cn = proto_close_encode(&cl, cb, sizeof cb);
	if (cn > 0) net_ctl_send(&ctl, PROTO_CTL_CLOSE, cb, (size_t)cn);
	vsend_destroy(D.vs);
	encoder_close(D.enc);
	if (touchdev) touch_close(touchdev);
	if (pendev) pen_close(pendev);
	vdisplay_close(D.vd);
	net_ctl_close(&ctl);
	if (!C.usb) close(ufd);
	else {
		close(ufd);
		usbmux_stop(mux);
		usblink_close(link);
	}
	return 0;
}
