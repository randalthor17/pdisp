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

#include "control.h"
#include "mdns.h"
#include "ratestep.h"
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
	const char *pair_cmd;    /* -A: command that asks the user (no terminal under systemd); see ask_command */
	const char *control_path; /* -C: the control socket (default $XDG_RUNTIME_DIR/pdisp/control.sock; "none" turns it off) */
	bool insecure;           /* development only: no handshake, no encryption (the M4 behaviour) */
	bool no_touch;           /* do not create a touchscreen for the phone */
	bool no_mdns;            /* do not announce this PC on the local network */
	const char *touch_hook;  /* a command run when the touchscreen exists, to bind it to the virtual output (docs/touch.md) */
	unsigned grace_secs;     /* after a lost link, keep the virtual output this long for the phone to come back (0 = tear it down at once) */
	bool once;               /* serve one session and exit instead of waiting for the next device */
	bool usb;          /* look for a USB cable (accessory mode); without -W this replaces listening on the network */
	bool also_net;     /* -W: with -u, listen on the network as well */
	unsigned usb_vid;  /* only consider this USB vendor (0 = any) */
	unsigned refine_frames, refine_min_qp, refine_start_qp, refine_rate_percent, refine_gap_ms, refine_settle_ms; /* static refinement (see D.refine_left); 0 frames = off */
} C = { .port = 47110, .max_hz_mhz = 60000, .bitrate = 12000, .max_bitrate = 40000, .name = "pdisp",
	.serial_text = "PDISP0001", .serial = 1, .card = -1, .grace_secs = 30 };

/* which settings the command line gave (they win over the settings file a client may change) */
enum { CONF_R = 1, CONF_B = 2, CONF_BMAX = 4, CONF_G = 8, CONF_F = 16 };
static unsigned g_cli_set;

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

/* Finding a refresh rate the device can sustain (ratestep.h): the cap is the user's; this is the session's own limit below it. */
static unsigned g_rate_limit_mhz;       /* 0 = none; else the highest rate this device was found able to sustain */
static unsigned g_idr_unres;            /* keyframes forced by unresolved frames so far */
static struct ratestep g_rs;
static struct proto_hello g_hello;      /* the device's HELLO, to pick another mode from */
static uint8_t g_dev_pub[PC_DH_LEN];    /* its key (secure sessions only) */
static bool g_have_dev_pub, g_stepped;  /* g_stepped: this session lowered the rate */
static char g_rates_path[1100];         /* "learned_rates" beside the settings file */

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
		g_idr_unres++;
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

/* A device connected while another one is being served: say so at once (a plain REJECT, reason 3, before any handshake) and hang up, instead of
 * leaving it in the listen queue to time out. */
static int g_lfd = -1; /* the listening socket (-1 when only a USB cable is served) */

static void refuse_busy(void)
{
	int fd = accept(g_lfd, NULL, NULL);
	if (fd < 0) return;
	struct net_ctl c;
	net_ctl_init(&c, fd);
	reject(&c, 3, "this PC is already serving another device");
	shutdown(fd, SHUT_WR); /* the REJECT is delivered before the close */
	net_ctl_close(&c);
}

/* The mode for a phone's display fields: its native size unless forced, the best refresh rate within the cap. Returns 0, or
 * a REJECT reason (with `*why`). */
static int choose_mode(const struct proto_hello *h, struct edid_params *ep, struct edid_timing *t, uint8_t edid[EDID_SIZE],
		       const char **why)
{
	unsigned w = C.force_w ? C.force_w : h->native_width, hh = C.force_h ? C.force_h : h->native_height;
	unsigned refresh_mhz = C.force_mhz;
	if (!refresh_mhz) {
		unsigned limit = C.max_hz_mhz; /* the user's cap is a ceiling; a rate this device was found unable to sustain lowers it for the session */
		if (g_rate_limit_mhz && g_rate_limit_mhz < limit) limit = g_rate_limit_mhz;
		for (int i = 0; i < h->n_refresh; i++)
			if (h->refresh_mhz[i] <= limit && h->refresh_mhz[i] > refresh_mhz) refresh_mhz = h->refresh_mhz[i];
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

static bool g_usb_conn; /* the connection being served arrived over the USB cable (C.usb only says that cables are looked for) */
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
	if (g_usb_conn) return send(fd, buf, n, MSG_DONTWAIT);
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
		"  -W          with -u: also listen on the network (a cable and Wi-Fi at the same time)\n"
		"  -u          serve a tablet over USB (AOA accessory mode) instead of the network; keeps waiting for the next\n"
		"              plug-in after each session unless -S is given\n"
		"  -V VID      with -u, only consider this USB vendor id in hex (e.g. 04e8)\n"
		"  -K DIR      identity and authorized_keys directory (default ~/.config/pdisp)\n"
		"  -P MODE     a device that is not authorised yet: ask (prompt on stdin, the default), accept (add it: development)\n"
		"  -A COMMAND  with -P ask: run COMMAND (through sh) to ask the user instead of reading stdin; PDISP_PEER_NAME, PDISP_PEER_KEY and PDISP_PAIR_CODE are set;\n"
		"              exit status 0 allows the device for good, 2 this time only, anything else denies (contrib/pdisp-pair-prompt)\n"
		"              or deny\n"
		"  -T          do not create a touchscreen for the phone's touches\n"
		"  -M          do not announce this PC on the local network (mDNS, so the app can list it)\n"
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
	while ((opt = getopt(argc, argv, "p:n:t:N:m:R:b:B:r:d:S:g:uWV:K:P:A:H:F:C:TMIho")) != -1) {
		switch (opt) {
		case 'p': C.port = (unsigned)atoi(optarg); break;
		case 'F': C.refine_frames = (unsigned)atoi(optarg); g_cli_set |= CONF_F; break; /* static refinement pictures; 0 = off */
		case 'n': C.name = optarg; break;
		case 't': C.serial_text = optarg; break;
		case 'N': C.serial = (uint32_t)strtoul(optarg, NULL, 0); break;
		case 'm': {
			double hz;
			if (sscanf(optarg, "%ux%u@%lf", &C.force_w, &C.force_h, &hz) != 3) { usage(argv[0]); return 2; }
			C.force_mhz = (unsigned)(hz * 1000 + 0.5);
			break;
		}
		case 'R': C.max_hz_mhz = (unsigned)(atof(optarg) * 1000); g_cli_set |= CONF_R; break;
		case 'b': C.bitrate = (unsigned)atoi(optarg); g_cli_set |= CONF_B; break;
		case 'B': C.max_bitrate = (unsigned)atoi(optarg); g_cli_set |= CONF_BMAX; break;
		case 'r': C.render_node = optarg; break;
		case 'd': C.card = atoi(optarg); break;
		case 'S': run_secs = atof(optarg); break;
		case 'u': C.usb = true; break;
		case 'W': C.also_net = true; break;
		case 'K': C.config_dir = optarg; break;
		case 'A': C.pair_cmd = optarg; break;
		case 'C': C.control_path = optarg; break;
		case 'P':
			if (!strcmp(optarg, "ask")) C.pair_mode = PAIR_ASK;
			else if (!strcmp(optarg, "accept")) C.pair_mode = PAIR_ACCEPT;
			else if (!strcmp(optarg, "deny")) C.pair_mode = PAIR_DENY;
			else { usage(argv[0]); return 2; }
			break;
		case 'I': C.insecure = true; break;
		case 'T': C.no_touch = true; break;
		case 'M': C.no_mdns = true; break;
		case 'H': C.touch_hook = optarg; break;
		case 'o': C.once = true; break;
		case 'g': C.grace_secs = (unsigned)atoi(optarg); g_cli_set |= CONF_G; break;
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

/* Runs the pairing prompt command (-A) through sh with PDISP_PEER_NAME, PDISP_PEER_KEY and PDISP_PAIR_CODE set, and waits for it: exit status 0 allows
 * the device for good, 2 allows it this time only, anything else denies. Returns the same letters as read_answer: y, o, n, or -1 on timeout. */
static int ask_command(const char *cmd, const char *name, const char *fp, unsigned code, int timeout_ms)
{
	pid_t pid = fork();
	if (pid < 0) return 'n';
	if (pid == 0) {
		char codebuf[16];
		snprintf(codebuf, sizeof codebuf, "%06u", code);
		setenv("PDISP_PEER_NAME", name, 1);
		setenv("PDISP_PEER_KEY", fp, 1);
		setenv("PDISP_PAIR_CODE", codebuf, 1);
		int dn = open("/dev/null", O_RDONLY);
		if (dn >= 0) { dup2(dn, 0); close(dn); }
		execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
		_exit(127);
	}
	uint64_t deadline = net_now_us() + (uint64_t)timeout_ms * 1000;
	int st = 0;
	for (;;) {
		pid_t w = waitpid(pid, &st, WNOHANG);
		if (w == pid) break;
		if (w < 0 && errno != EINTR) return 'n';
		if (stop || net_now_us() >= deadline) {
			kill(pid, SIGTERM);
			waitpid(pid, &st, 0);
			return stop ? 'n' : -1;
		}
		struct timespec ts = { 0, 50 * 1000 * 1000 };
		nanosleep(&ts, NULL);
	}
	if (!WIFEXITED(st)) return 'n';
	return WEXITSTATUS(st) == 0 ? 'y' : WEXITSTATUS(st) == 2 ? 'o' : 'n';
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
	int a = control_pair_ask(peer->name, fp, peer->sas, 90000, &stop); /* a GUI that listens for pairing requests asks the user */
	if (a != -2) {
		/* answered (or timed out) there */
	} else if (C.pair_cmd) {
		a = ask_command(C.pair_cmd, peer->name, fp, peer->sas, 90000);
	} else {
	fprintf(stderr, "\nA device wants to use this PC as a second monitor.\n   name: %s\n   key:  %s\n   code: %06u   (it must be the same on the device's screen)\n"
			"Allow it?  [y] always  [o] this time only  [N] no: ", peer->name, fp, peer->sas);
		fflush(stderr);
		a = read_answer(90000);
	}
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
static struct mdns *g_mdns;
static char g_ks_path[1100];

/* ================= the control socket (docs/control.md) =================
 * The state a GUI shows, kept up to date by the streaming loop; control.c copies it into its own thread. */
static struct ctl_state g_cs;
static volatile sig_atomic_t g_disconnect_req; /* a client asked to end the session */
static bool g_grace_active;                    /* the monitor is being kept for a lost device */
static uint64_t g_session_start_us, g_pub_frames, g_pub_us;
static char g_conf_path[1100];                 /* the settings file a client can change; read at start */

/* ---- the rates learned per device and transport: "<first 16 bytes of its key in hex><u or n> <rate Hz> <the cap in Hz when it was learned>" per line.
 * The device decides what its decoder can do and the transport what the link can: a rate that works over USB says nothing about Wi-Fi. ---- */
#define RATES_KEY_LEN 33
static void rates_key(const uint8_t pub[PC_DH_LEN], bool usb, char out[RATES_KEY_LEN + 1])
{
	for (int i = 0; i < 16; i++) snprintf(out + 2 * i, 3, "%02x", pub[i]);
	out[32] = usb ? 'u' : 'n';
	out[33] = 0;
}

/* The rate known to be sustainable for this device, if it was learned under a cap at least as high as today's (a higher cap is a new question). */
static unsigned rates_load(const uint8_t pub[PC_DH_LEN], bool usb)
{
	if (!g_rates_path[0]) return 0;
	FILE *f = fopen(g_rates_path, "r");
	if (!f) return 0;
	char key[RATES_KEY_LEN + 1], line[200];
	unsigned hz = 0, cap, found = 0;
	rates_key(pub, usb, key);
	while (fgets(line, sizeof line, f)) {
		char k[40];
		if (sscanf(line, "%39s %u %u", k, &hz, &cap) == 3 && !strcmp(k, key)) { found = (C.max_hz_mhz / 1000 <= cap && hz >= 24) ? hz : 0; break; }
	}
	fclose(f);
	return found;
}

static void rates_save(const uint8_t pub[PC_DH_LEN], bool usb, unsigned hz, unsigned cap)
{
	if (!g_rates_path[0]) return;
	char key[RATES_KEY_LEN + 1], tmp[1120], line[200];
	rates_key(pub, usb, key);
	snprintf(tmp, sizeof tmp, "%s.tmp", g_rates_path);
	FILE *o = fopen(tmp, "w");
	if (!o) return;
	FILE *f = fopen(g_rates_path, "r");
	while (f && fgets(line, sizeof line, f)) if (strncmp(line, key, RATES_KEY_LEN)) fputs(line, o);
	fprintf(o, "%s %u %u\n", key, hz, cap);
	if (f) fclose(f);
	fclose(o);
	if (rename(tmp, g_rates_path)) unlink(tmp);
}

static void rates_clear(void) { if (g_rates_path[0]) unlink(g_rates_path); }


static void ctl_publish(void)
{
	g_cs.usb = C.usb;
	g_cs.net = g_lfd >= 0;
	g_cs.port = C.port;
	g_cs.monitor = D.vd != NULL;
	if (g_cs.monitor) connector_name(g_cs.connector, sizeof g_cs.connector); else g_cs.connector[0] = 0;
	g_cs.grace = g_grace_active && g_cs.monitor && !g_cs.session;
	g_cs.refresh_cap_hz = C.max_hz_mhz / 1000;
	g_cs.bitrate_kbps = C.bitrate;
	g_cs.max_bitrate_kbps = C.max_bitrate;
	g_cs.grace_secs = C.grace_secs;
	g_cs.refine_frames = C.refine_frames;
	control_publish(&g_cs);
}

static void ctl_set_usb_state(const char *s)
{
	snprintf(g_cs.usb_state, sizeof g_cs.usb_state, "%s", C.usb ? s : "off");
	ctl_publish();
}

/* The live numbers of the session being served, called about once a second. */
static void ctl_session_numbers(uint64_t now)
{
	const struct vsend_stats *st = vsend_stats(D.vs);
	g_cs.width = D.width;
	g_cs.height = D.height;
	g_cs.refresh_hz = D.refresh_mhz / 1000.0;
	g_cs.session_secs = (now - g_session_start_us) / 1e6;
	double dt = (now - g_pub_us) / 1e6;
	if (dt > 0.2) { g_cs.fps = (double)(D.frames_encoded - g_pub_frames) / dt; g_pub_frames = D.frames_encoded; g_pub_us = now; }
	g_cs.bitrate_mbps = vsend_bitrate_kbps(D.vs) / 1000.0;
	g_cs.queue_ms = vsend_queue_delay_us(D.vs) / 1000.0;
	g_cs.phone_queue_ms = st->fb_queue_max_us / 1000.0;
	g_cs.phone_decode_ms = st->fb_decode_max_x10 / 10.0;
	g_cs.frames_lost = st->frames_lost;
	g_cs.idr_frames = D.idr_frames;
	g_cs.repair_packets = st->repair_packets;
	ctl_publish();
}

static void ctl_session_begin(const char *device)
{
	snprintf(g_cs.device, sizeof g_cs.device, "%s", device);
	snprintf(g_cs.transport, sizeof g_cs.transport, "%s", g_usb_conn ? "usb" : "network");
	g_cs.session = true;
	g_session_start_us = g_pub_us = net_now_us();
	g_pub_frames = D.frames_encoded;
	ctl_session_numbers(g_pub_us);
}

static void ctl_session_end(void)
{
	g_cs.session = false;
	g_cs.fps = g_cs.bitrate_mbps = g_cs.queue_ms = 0;
	ctl_publish();
}

/* ---- the settings file: key=value lines, written by set_config, read at start for what the command line did not give */
static void conf_load(void)
{
	FILE *f = g_conf_path[0] ? fopen(g_conf_path, "r") : NULL;
	if (!f) return;
	char line[128];
	while (fgets(line, sizeof line, f)) {
		char *eq = strchr(line, '=');
		if (!eq || line[0] == '#') continue;
		*eq = 0;
		unsigned v = (unsigned)strtoul(eq + 1, NULL, 10);
		if (!strcmp(line, "refresh_cap_hz") && !(g_cli_set & CONF_R) && v >= 24 && v <= 480) C.max_hz_mhz = v * 1000;
		else if (!strcmp(line, "bitrate_kbps") && !(g_cli_set & CONF_B) && v >= 1000 && v <= 200000) C.bitrate = v;
		else if (!strcmp(line, "max_bitrate_kbps") && !(g_cli_set & CONF_BMAX) && v >= 2000 && v <= 200000) C.max_bitrate = v;
		else if (!strcmp(line, "grace_secs") && !(g_cli_set & CONF_G) && v <= 3600) C.grace_secs = v;
		else if (!strcmp(line, "refine_frames") && !(g_cli_set & CONF_F) && v <= 400) C.refine_frames = v;
	}
	fclose(f);
}

static int conf_save(void)
{
	if (!g_conf_path[0]) return -ENOENT;
	char tmp[1120];
	snprintf(tmp, sizeof tmp, "%s.tmp", g_conf_path);
	FILE *f = fopen(tmp, "w");
	if (!f) return -errno;
	fprintf(f, "# written by pdisp: settings changed through the control socket\nrefresh_cap_hz=%u\nbitrate_kbps=%u\nmax_bitrate_kbps=%u\ngrace_secs=%u\nrefine_frames=%u\n",
		C.max_hz_mhz / 1000, C.bitrate, C.max_bitrate, C.grace_secs, C.refine_frames);
	if (fclose(f) != 0 || rename(tmp, g_conf_path) != 0) return -errno;
	return 0;
}

static void op_disconnect(void *user) { (void)user; g_disconnect_req = 1; }

static void op_visit(const struct keystore_entry *e, void *user)
{
	struct jw *w = user;
	char fp[IDENTITY_FINGERPRINT_LEN];
	identity_fingerprint(e->pub, fp);
	jw_obj_begin(w);
	jw_str(w, "fingerprint", fp);
	jw_str(w, "name", e->name);
	jw_obj_end(w);
}

static int op_devices(void *user, struct jw *w)
{
	(void)user;
	jw_arr_begin(w, "devices");
	if (g_ks_path[0]) keystore_list(g_ks_path, op_visit, w);
	jw_arr_end(w);
	return 0;
}

static int op_remove_device(void *user, const char *prefix)
{
	(void)user;
	if (strlen(prefix) < 4 || !g_ks_path[0]) return -EINVAL;
	return keystore_remove(g_ks_path, prefix);
}

static int op_set_config(void *user, const char *key, const char *value, char *err, size_t errcap)
{
	(void)user;
	char *end;
	unsigned long v = strtoul(value, &end, 10);
	if (*end || !*value) { snprintf(err, errcap, "%s must be a whole number", key); return -EINVAL; }
	if (!strcmp(key, "refresh_cap_hz")) { if (v < 24 || v > 480) goto range; C.max_hz_mhz = (unsigned)v * 1000; rates_clear(); /* a new cap: find the stable rates again */ }
	else if (!strcmp(key, "bitrate_kbps")) { if (v < 1000 || v > 200000) goto range; C.bitrate = (unsigned)v; }
	else if (!strcmp(key, "max_bitrate_kbps")) { if (v < 2000 || v > 200000) goto range; C.max_bitrate = (unsigned)v; }
	else if (!strcmp(key, "grace_secs")) { if (v > 3600) goto range; C.grace_secs = (unsigned)v; }
	else if (!strcmp(key, "refine_frames")) { if (v > 400) goto range; C.refine_frames = (unsigned)v; }
	else { snprintf(err, errcap, "unknown setting %s", key); return -EINVAL; }
	if (conf_save() != 0) LOG("cannot save the settings to %s", g_conf_path);
	LOG("setting %s = %lu (from a client; used from the next session on)", key, v);
	ctl_publish();
	return 0;
range:
	snprintf(err, errcap, "%s is out of range", key);
	return -ERANGE;
}

static const struct control_ops g_control_ops = { .disconnect = op_disconnect, .devices = op_devices, .remove_device = op_remove_device, .set_config = op_set_config };
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
		g_usb_conn = false;
		ctl_set_usb_state("waiting");
		for (;;) { /* in short slices, so the display keeps being serviced and the deadline is honoured; a cable and the network are both watched when both are on */
			if (stop) return -ECANCELED;
			if (g_disconnect_req) { g_disconnect_req = 0; return -ECONNABORTED; } /* a client asked to remove the monitor that is waiting for a lost device */
			if (deadline_us && net_now_us() >= deadline_us) return -ETIMEDOUT;
			if (g_lfd >= 0) {
				struct pollfd lp[2] = { { .fd = g_lfd, .events = POLLIN }, { .fd = D.vd ? vdisplay_event_fd(D.vd) : -1, .events = POLLIN } };
				poll(lp, 2, C.usb ? 100 : 200);
				service_display();
				if (lp[0].revents & POLLIN) {
					c->cfd = accept(g_lfd, NULL, NULL);
					if (c->cfd < 0) { if (errno == EINTR || errno == EAGAIN) continue; perror("accept"); return -errno; }
					net_tune_tcp(c->cfd);
					break;
				}
			} else {
				service_display();
			}
			if (C.usb) {
				int rd, wr;
				int e = usblink_open(g_lfd >= 0 ? 300 : 500, C.usb_vid, (const volatile int *)&stop, &c->link, &rd, &wr);
				if (e == -ETIMEDOUT) continue;
				if (e == -ECANCELED) return e;
				if (e) { LOG("cannot open the USB device: %s (is the udev rule installed? see contrib/udev)", strerror(-e)); return e; }
				LOG("USB accessory %s", usblink_description(c->link));
				ctl_set_usb_state("accessory");
				int usb_dgram;
				e = usbmux_start(rd, wr, 0, &c->mux, &c->cfd, &usb_dgram);
				if (e) { LOG("cannot start the USB multiplexer: %s", strerror(-e)); usblink_close(c->link); c->link = NULL; return e; }
				fcntl(usb_dgram, F_SETFL, fcntl(usb_dgram, F_GETFL) | O_NONBLOCK);
				c->ufd = usb_dgram;
				g_usb_conn = true;
				break;
			}
		}
		net_ctl_init(&c->ctl, c->cfd);
		if (g_usb_conn && g_lfd >= 0 && !C.insecure) {
			/* A cable whose app is not running sends nothing: do not keep a network device waiting behind it for the whole handshake timeout. */
			bool alive = false, net_waiting = false;
			for (int waited = 0; waited < 10000 && !alive && !net_waiting; waited += 200) {
				if (stop) { conn_close(c); return -ECANCELED; }
				struct pollfd pw[2] = { { .fd = c->cfd, .events = POLLIN }, { .fd = g_lfd, .events = POLLIN } };
				poll(pw, 2, 200);
				service_display();
				alive = pw[0].revents != 0;
				net_waiting = (pw[1].revents & POLLIN) != 0;
			}
			if (!alive) {
				LOG(net_waiting ? "a network device is waiting: leaving the silent USB peer" : "a peer failed the handshake (no answer over USB): dropped");
				conn_close(c);
				continue;
			}
		}
		if (C.insecure) return 0;
		int r = hs_respond(&c->ctl, &g_id, C.name, 10000, &c->dev, &secudp);
		if (r == 0) return 0;
		LOG("a peer failed the handshake (%s): dropped", strerror(-r));
		conn_close(c);
	}
}

/* One phone, from its HELLO to the end of the stream. The virtual output and encoder outlive this call (see above). */
/* Rebuilds the output, encoder and sender for a new mode and tells the device (RECONFIGURE). 0, 1 (the link is unusable) or 2 (fatal). */
static int reconfigure(struct net_ctl *ctl, const struct proto_hello *nh, const struct edid_params *nep, const struct edid_timing *nt,
		       const uint8_t nedid[EDID_SIZE])
{
	D.streaming = false;
	D.refresh_due_us = 0;
	D.refine_due_us = 0;
	if (bring_up(nep, nt, nedid) != 0 || ++D.epoch == 0) { LOG("reconfiguration failed"); return 1; }
	touch_start(nh, D.width, D.height, nep); /* the coordinate range may have changed with the size */
	if (make_sender() != 0) return 2;
	if (!C.insecure) sec_udp_set_epoch(&secudp, D.epoch); /* sequence numbers restart: new datagram keys */
	struct proto_stream_params rp;
	make_params(&rp);
	uint8_t rb[1400];
	int rn = proto_stream_params_encode(&rp, rb, sizeof rb);
	if (rn < 0 || net_ctl_send(ctl, PROTO_CTL_RECONFIGURE, rb, (size_t)rn) != 0) return 1;
	D.awaiting_ack = true;
	D.ack_deadline_us = net_now_us() + 10000000;
	D.reconfigs++;
	ratestep_reset(&g_rs);
	return 0;
}

static enum end_reason serve(struct conn *c, double run_secs)
{
	struct net_ctl *ctl = &c->ctl;
	uint16_t type;
	const uint8_t *payload;
	size_t plen;
	struct proto_hello hello;
	if (net_ctl_recv(ctl, &type, &payload, &plen, g_usb_conn ? 120000 : 10000) != 1 || type != PROTO_CTL_HELLO ||
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

	g_hello = hello;
	g_have_dev_pub = !C.insecure;
	if (g_have_dev_pub) memcpy(g_dev_pub, c->dev.pub, PC_DH_LEN);
	g_rate_limit_mhz = g_have_dev_pub ? rates_load(g_dev_pub, g_usb_conn) * 1000 : 0; /* what this device was found able to sustain, under today's cap */
	if (g_rate_limit_mhz) LOG("this device sustains %u Hz over %s (found earlier): starting there", g_rate_limit_mhz / 1000, g_usb_conn ? "USB" : "the network");
	g_stepped = false;
	g_idr_unres = 0;
	ratestep_reset(&g_rs);
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
	if (!g_usb_conn) {
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
	bool have_peer = g_usb_conn, ready = false; /* over USB the datagram socket is already connected */
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
	ctl_session_begin(hello.device_name);

	/* ---- stream: the first frame is the current screen, so a static desktop shows up at once */
	D.streaming = true;
	struct vdisplay_frame cur;
	if (vdisplay_current_frame(D.vd, &cur)) encode_and_submit(cur.data, cur.stride, net_now_us(), PIC_NEW);
	else D.refresh_due_us = net_now_us() + 100000;

	enum end_reason end = END_LINK;
	uint64_t last_rx = net_now_us(), end_us = run_secs > 0 ? net_now_us() + (uint64_t)(run_secs * 1e6) : 0;
	uint64_t last_pub = net_now_us();
	uint64_t last_report = net_now_us(), next_ping = net_now_us() + 2000000, next_usb_nudge = net_now_us() + 3000000, last_pong = net_now_us();
	int evfd = vdisplay_event_fd(D.vd);
	uint8_t pkt[2048];
	int done = 0;
	while (!done) {
		if (stop) { end = END_STOP; break; }
		uint64_t now = net_now_us();
		if (g_disconnect_req) { g_disconnect_req = 0; LOG("a client asked to end the session"); end = END_USER; break; }
		if (now - last_pub >= 1000000) { last_pub = now; ctl_session_numbers(now); }
		if (end_us && now >= end_us) { end = END_STOP; break; }
		uint64_t due = vsend_next_due_us(D.vs, now);
		int timeout_ms = due <= now ? 0 : (int)((due - now + 999) / 1000);
		if (timeout_ms > 10) timeout_ms = 10;
		struct pollfd p[4] = { { .fd = evfd, .events = POLLIN }, { .fd = ufd, .events = POLLIN }, { .fd = c->cfd, .events = POLLIN }, { .fd = g_lfd, .events = POLLIN } };
		uint64_t t_a = net_now_us();
		poll(p, 4, timeout_ms);
		if (g_lfd >= 0 && (p[3].revents & POLLIN)) refuse_busy(); /* a second device must not wait in the listen queue until this session ends */
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
			if (!g_usb_conn && (from.sin_addr.s_addr != peer->sin_addr.s_addr || from.sin_port != peer->sin_port)) continue;
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
						g_hello = nh;
						int rr = reconfigure(ctl, &nh, &nep, &nt, nedid);
						if (rr) { end = rr == 2 ? END_FATAL : END_LINK; done = 1; break; }
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
		if (C.usb && !g_usb_conn && now > next_usb_nudge) { /* a network session, and a cable may be plugged in: move to it (the app switches when the accessory appears) */
			next_usb_nudge = now + 3000000;
			if (usblink_nudge(C.usb_vid) > 0) LOG("an Android device is plugged into this PC: asking it to switch to the cable");
		}
		if (now - last_pong > 8000000 && D.pongs) { LOG("no PONG for 8 s: the control channel is dead"); end = END_LINK; break; }
		if (g_usb_conn && usblink_gone(c->link)) { LOG("the USB cable was unplugged"); end = END_LINK; break; }
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
			const unsigned fb_dec_x10 = st->fb_decode_max_x10; /* the feedback window is reset below */
			LOG("frames %llu (skipped %llu, idr %llu), %.1f Mbit/s target, FEC %u%%, repairs %llu, nacks %llu, lost %llu, "
			    "queue %.1f ms, unresolved %u, phone: queue %.1f ms decode %.1f ms recv >= %.1f Mbit/s",
			    (unsigned long long)D.frames_encoded, (unsigned long long)D.frames_skipped, (unsigned long long)D.idr_frames,
			    vsend_bitrate_kbps(D.vs) / 1000.0, vsend_fec_percent(D.vs), (unsigned long long)st->repair_packets,
			    (unsigned long long)st->nacks, (unsigned long long)st->frames_lost, vsend_queue_delay_us(D.vs) / 1000.0,
			    vsend_unresolved(D.vs), st->fb_queue_max_us / 1000.0, st->fb_decode_max_x10 / 10.0, st->fb_recv_kbps_min / 1000.0);
			vsend_reset_report(D.vs);

			/* is this rate sustainable? A quiet screen proves nothing; trouble over most of 12 s steps down to the next rate the device offers */
			static uint64_t prev_frames, prev_lost, prev_pkts, prev_rep;
			static unsigned prev_idr;
			struct ratestep_sample rs = { .active = D.frames_encoded > prev_frames && D.streaming, .idr_unresolved = g_idr_unres - prev_idr,
						      .frames_lost = (unsigned)(st->frames_lost - prev_lost), .phone_slow = fb_dec_x10 >= 500,
						      .low_rate = vsend_bitrate_kbps(D.vs) < 6000,
						      .heavy_repair = (st->repair_packets - prev_rep) * 100 > (st->packets - prev_pkts) * 12 && st->packets > prev_pkts + 50 };
			prev_frames = D.frames_encoded; prev_lost = st->frames_lost; prev_idr = g_idr_unres; prev_pkts = st->packets; prev_rep = st->repair_packets;
			enum ratestep_action act = D.awaiting_ack ? RATESTEP_NONE : ratestep_tick(&g_rs, &rs);
			if (act == RATESTEP_DOWN) {
				unsigned lower = ratestep_next_lower(g_hello.refresh_mhz, g_hello.n_refresh, D.refresh_mhz, 30000);
				struct edid_params nep;
				struct edid_timing nt;
				uint8_t nedid[EDID_SIZE];
				const char *nwhy = "";
				if (!lower) LOG("the stream is not healthy at %u Hz and the device offers nothing lower", D.refresh_mhz / 1000);
				else {
					LOG("the stream is not healthy at %u Hz (keyframes forced %u, lost %u, decode up to %.0f ms, target %.1f Mbit/s, repairs %s): trying %u Hz",
					    D.refresh_mhz / 1000, rs.idr_unresolved, rs.frames_lost, fb_dec_x10 / 10.0, vsend_bitrate_kbps(D.vs) / 1000.0, rs.heavy_repair ? "heavy" : "light", lower / 1000);
					g_rate_limit_mhz = lower;
					g_stepped = true;
					if (choose_mode(&g_hello, &nep, &nt, nedid, &nwhy) == 0) {
						int rr = reconfigure(ctl, &g_hello, &nep, &nt, nedid);
						if (rr) { end = rr == 2 ? END_FATAL : END_LINK; break; }
						evfd = vdisplay_event_fd(D.vd);
					}
				}
			} else if (act == RATESTEP_STABLE && g_stepped && g_have_dev_pub) {
				LOG("%u Hz is stable for this device: remembering it", D.refresh_mhz / 1000);
				rates_save(g_dev_pub, g_usb_conn, D.refresh_mhz / 1000, C.max_hz_mhz / 1000);
			}
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
		snprintf(g_conf_path, sizeof g_conf_path, "%s/pdispd.conf", cdir);
		snprintf(g_rates_path, sizeof g_rates_path, "%s/learned_rates", cdir);
		conf_load();
	}
	if (!C.usb || C.also_net) {
		g_lfd = net_tcp_listen(C.port);
		if (g_lfd < 0) { fprintf(stderr, "cannot listen on %u: %s\n", C.port, strerror(-g_lfd)); return 1; }
		LOG("listening on port %u", C.port);
		if (!C.no_mdns) {
			char host[64] = "", label[64] = "pdisp", inst[64], fp[IDENTITY_FINGERPRINT_LEN], fptxt[40];
			gethostname(host, sizeof host - 1);
			size_t j = 5;
			for (const char *h = host; *h && j < sizeof label - 1; h++) { /* a DNS label: letters, digits and hyphens */
				if (j == 5) label[j++] = '-';
				label[j++] = (*h >= 'A' && *h <= 'Z') ? *h + 32 : ((*h >= 'a' && *h <= 'z') || (*h >= '0' && *h <= '9')) ? *h : '-';
			}
			label[j] = 0;
			snprintf(inst, sizeof inst, "pdisp on %s", host[0] ? host : "this PC");
			const char *txt[3] = { "v=1", NULL, NULL };
			if (!C.insecure) { identity_fingerprint(g_id.pub, fp); snprintf(fptxt, sizeof fptxt, "fp=%s", fp); txt[1] = fptxt; }
			struct mdns_svc ms;
			mdns_svc_init(&ms, inst, label, (uint16_t)C.port, txt);
			int me = mdns_start(&ms, &g_mdns);
			if (me == 0) LOG("announcing \"%s\" on the local network (mDNS)", inst);
			else LOG("not announcing on the local network: %s", strerror(-me));
		}
	} else {
		LOG("waiting for a tablet on USB");
	}
	{ /* the control socket for a GUI or a script (docs/control.md) */
		char cpath[200] = "";
		const char *rt = getenv("XDG_RUNTIME_DIR");
		if (C.control_path) snprintf(cpath, sizeof cpath, "%s", C.control_path);
		else if (rt && *rt) snprintf(cpath, sizeof cpath, "%s/pdisp/control.sock", rt);
		if (cpath[0] && strcmp(cpath, "none")) {
			int ce = control_start(cpath, &g_control_ops);
			if (ce == 0) LOG("control socket %s", cpath); else LOG("no control socket (%s): %s", cpath, strerror(-ce));
		}
		ctl_set_usb_state("waiting");
	}

	bool single = C.once || run_secs > 0; /* -o and -S serve one session and exit */
	uint64_t grace_deadline = 0; /* while the monitor waits for a lost phone: when it gives up */
	int exit_code = 0;
	for (;;) {
		struct conn c;
		int r = acquire_peer(&c, grace_deadline);
		if (r == -ECANCELED) break;
		if (r == -ECONNABORTED) { LOG("a client removed the waiting monitor"); teardown_output(); grace_deadline = 0; g_grace_active = false; ctl_publish(); continue; }
		if (r == -ETIMEDOUT) {
			LOG("the phone did not come back within %u s", C.grace_secs);
			teardown_output();
			grace_deadline = 0;
			g_grace_active = false;
			ctl_publish();
			if (single) break;
			continue;
		}
		if (r) { exit_code = 1; break; }
		enum end_reason end = serve(&c, run_secs);
		ctl_session_end();
		conn_close(&c);
		if (end == END_FATAL) { exit_code = g_exit_code ? g_exit_code : 1; break; }
		if (end == END_STOP) break;
		if (end == END_REJECTED) { if (single) break; continue; } /* the monitor and any grace period are as they were */
		if (single) break;
		if (end == END_LINK && C.grace_secs && D.vd) {
			grace_deadline = net_now_us() + (uint64_t)C.grace_secs * 1000000;
			g_grace_active = true;
			ctl_publish();
			LOG("lost the phone: keeping the virtual output for %u s in case it comes back", C.grace_secs);
		} else {
			teardown_output(); /* on purpose, or its screen locked, or no grace period: the compositor moves the windows away now */
			grace_deadline = 0;
			g_grace_active = false;
			ctl_publish();
		}
	}
	mdns_stop(g_mdns); g_mdns = NULL;
	teardown_output();
	control_stop();
	if (g_lfd >= 0) close(g_lfd);
	return exit_code;
}
