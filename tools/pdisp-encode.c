/* pdisp-encode: capture a virtual monitor and encode every update to an HEVC file (M3 pass check).
 *
 *   pdisp-encode [EDID options] -o out.hevc -S SECONDS [-b KBPS] [-V MS] [-k N] [-r NODE] [-v]
 *
 * Exit: 0 success, 1 failure, 2 usage, 3 no free evdi device.
 */
#include "edid_cli.h"
#include "encoder.h"
#include "vdisplay.h"

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stop;
static void on_signal(int sig) { (void)sig; stop = 1; }

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}
static double t0;
#define LOG(...) do { printf("[%7.3f] ", now() - t0); printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while (0)

struct stats {
	long frames, idr;
	double bytes_i, bytes_p, max_i, max_p;
	double upload_sum, convert_sum, encode_sum;
	double upload_max, convert_max, encode_max;
	double total_max; /* upload + convert + encode */
};

static struct {
	struct encoder *enc;
	struct encoder_config cfg;
	FILE *out;
	bool verbose;
	unsigned force_idr_every;
	bool failed;
	struct stats st;
	long pending_idr;
} A;

static void on_frame(const struct vdisplay_frame *f, void *u)
{
	(void)u;
	if (A.failed) return;

	if (!A.enc) {
		/* the first frame tells us the real mode; open the encoder for it */
		A.cfg.width = f->width;
		A.cfg.height = f->height;
		int rc = encoder_open(&A.cfg, &A.enc);
		if (rc) {
			fprintf(stderr, "cannot open encoder (%dx%d): %s\n", f->width, f->height, strerror(-rc));
			A.failed = true;
			return;
		}
		LOG("encoder: %s, %ux%u, %u kbit/s %s, vbv %u ms", encoder_description(A.enc), A.cfg.width, A.cfg.height,
		    A.cfg.bitrate_kbps, A.cfg.vbr ? "vbr" : "cbr", A.cfg.vbv_ms);
	}
	if ((unsigned)f->width != A.cfg.width || (unsigned)f->height != A.cfg.height) return; /* mode changed: M9 */

	bool idr = A.force_idr_every && A.st.frames > 0 && (A.st.frames % A.force_idr_every) == 0;
	struct encoder_packet p;
	int rc = encoder_encode(A.enc, f->data, f->stride, idr, &p);
	if (rc) {
		fprintf(stderr, "encode failed: %s\n", strerror(-rc));
		A.failed = true;
		return;
	}
	if (fwrite(p.data, 1, p.size, A.out) != p.size) {
		perror("write");
		A.failed = true;
		return;
	}

	struct stats *s = &A.st;
	s->frames++;
	if (p.idr) { s->idr++; s->bytes_i += p.size; if (p.size > s->max_i) s->max_i = p.size; }
	else { s->bytes_p += p.size; if (p.size > s->max_p) s->max_p = p.size; }
	s->upload_sum += p.upload_ms; s->convert_sum += p.convert_ms; s->encode_sum += p.encode_ms;
	if (p.upload_ms > s->upload_max) s->upload_max = p.upload_ms;
	if (p.convert_ms > s->convert_max) s->convert_max = p.convert_ms;
	if (p.encode_ms > s->encode_max) s->encode_max = p.encode_ms;
	double total = p.upload_ms + p.convert_ms + p.encode_ms;
	if (total > s->total_max) s->total_max = total;
	if (A.verbose)
		LOG("frame #%ld %s %zu bytes upload=%.2f convert=%.2f encode=%.2f ms", s->frames, p.idr ? "IDR" : "P  ",
		    p.size, p.upload_ms, p.convert_ms, p.encode_ms);
}

static void usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s [options] -o FILE -S SECONDS\n" EDID_CLI_USAGE
		"  -o FILE     raw HEVC (Annex B) output\n"
		"  -S SECS     capture and encode for this long\n"
		"  -b KBPS     bitrate: the rate for cbr, the peak for vbr (default 20000)\n"
		"  -R MODE     vbr (default) or cbr\n"
		"  -T PCT      vbr average as a percentage of the peak (default 50)\n"
		"  -V MS       rate-control buffer in ms (default 30)\n"
		"  -k N        force an IDR every N frames (default: never, only the first frame)\n"
		"  -I N        rolling intra refresh sweeping the picture in about N frames (default: off)\n"
		"  -L N        slices per picture (default 1)\n"
		"  -r NODE     VAAPI render node (default: first that can encode HEVC)\n"
		"  -d CARD     evdi card (default: first free)\n"
		"  -v          log every frame\n",
		argv0);
}

int encode_main(int argc, char **argv)
{
	struct edid_params p;
	const char *outfile = NULL;
	double secs = 0;
	int card = -1, opt;

	edid_cli_defaults(&p);
	A.cfg = (struct encoder_config){ .fps = 60, .bitrate_kbps = 20000, .vbv_ms = 30, .vbr = true, .target_percent = 50 };
	while ((opt = getopt(argc, argv, EDID_CLI_OPTS "o:S:b:V:k:r:d:R:T:I:L:vh")) != -1) {
		switch (opt) {
		case 'o': outfile = optarg; break;
		case 'S': secs = atof(optarg); break;
		case 'b': A.cfg.bitrate_kbps = (unsigned)atoi(optarg); break;
		case 'V': A.cfg.vbv_ms = (unsigned)atoi(optarg); break;
		case 'k': A.force_idr_every = (unsigned)atoi(optarg); break;
		case 'r': A.cfg.render_node = optarg; break;
		case 'd': card = atoi(optarg); break;
		case 'R': A.cfg.vbr = strcmp(optarg, "cbr") != 0; break;
		case 'T': A.cfg.target_percent = (unsigned)atoi(optarg); break;
		case 'I': A.cfg.intra_refresh_frames = (unsigned)atoi(optarg); break;
		case 'L': A.cfg.slices = (unsigned)atoi(optarg); break;
		case 'v': A.verbose = true; break;
		default:
			if (edid_cli_opt(&p, opt, optarg) != 1) { usage(argv[0]); return 2; }
		}
	}
	if (!outfile || secs <= 0) { usage(argv[0]); return 2; }

	uint8_t edid[EDID_SIZE];
	struct edid_timing timing;
	if (edid_generate(&p, edid) != 0 || edid_compute_timing(p.width, p.height, p.refresh_mhz, &timing) != 0) {
		fprintf(stderr, "%s: parameters cannot be encoded in an EDID base block\n", argv[0]);
		return 1;
	}
	A.cfg.fps = (unsigned)(timing.refresh_hz + 0.5);

	struct sigaction sa = { .sa_handler = on_signal };
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	t0 = now();

	A.out = fopen(outfile, "wb");
	if (!A.out) { perror(outfile); return 1; }

	if (card < 0) card = vdisplay_find_free_device();
	if (card < 0) {
		fprintf(stderr, "no free evdi device (create one: echo 1 > /sys/devices/evdi/add, as root)\n");
		return 3;
	}
	struct vdisplay_events ev = { .frame = on_frame };
	struct vdisplay *vd;
	int err = vdisplay_open(card, &ev, &vd);
	if (err) { fprintf(stderr, "cannot open /dev/dri/card%d: %s\n", card, strerror(-err)); return 1; }
	if (vdisplay_connect(vd, edid, timing.hactive, timing.vactive, timing.pixel_clock_khz) != 0) {
		vdisplay_close(vd);
		return 1;
	}
	LOG("connected '%s' serial '%s' %ux%u@%.3f on card%d", p.name, p.serial_text, p.width, p.height, timing.refresh_hz, card);

	double end = now() + secs;
	while (!stop && !A.failed && now() < end) vdisplay_pump(vd, 50);

	vdisplay_close(vd);
	if (A.out) fclose(A.out);

	struct stats *s = &A.st;
	long np = s->frames - s->idr;
	if (A.failed || s->frames == 0) {
		fprintf(stderr, "FAIL: %s\n", A.failed ? "encoder error" : "no frames were encoded");
		if (A.enc) encoder_close(A.enc);
		return 1;
	}
	double span = secs;
	printf("ENCODE frames=%ld idr=%ld fps=%.1f bitrate_kbps=%.0f\n", s->frames, s->idr, s->frames / span,
	       (s->bytes_i + s->bytes_p) * 8 / 1000 / span);
	printf("ENCODE_MS upload avg=%.2f max=%.2f convert avg=%.2f max=%.2f encode avg=%.2f max=%.2f total_max=%.2f\n",
	       s->upload_sum / s->frames, s->upload_max, s->convert_sum / s->frames, s->convert_max,
	       s->encode_sum / s->frames, s->encode_max, s->total_max);
	printf("ENCODE_SIZE I avg=%.0f max=%.0f bytes, P avg=%.0f max=%.0f bytes\n", s->idr ? s->bytes_i / s->idr : 0.0,
	       s->max_i, np ? s->bytes_p / np : 0.0, s->max_p);
	if (A.enc) encoder_close(A.enc);
	return 0;
}
