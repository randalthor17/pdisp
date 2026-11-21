/* pdisp-capture: connect an evdi virtual monitor and capture what the compositor draws on it.
 *
 *   -V   wait until a captured frame equals the test pattern (tools/pattern.h) bit-exactly
 *   -o   write the matching frame (or, on failure, the last frame) as a PNG
 *   -S   after that (or from the start without -V), keep capturing for SECS seconds and print statistics
 *   -v   log every frame
 *
 * Exit: 0 success, 1 failure (no match before -T timeout), 2 usage, 3 no free evdi device.
 */
#include "edid_cli.h"
#include "pattern.h"
#include "png.h"
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

static struct {
	bool verify, verbose;
	const char *png_path;
	double stats_secs;

	/* reference pattern for the current mode */
	uint32_t *ref;
	int ref_w, ref_h;

	/* verification result */
	bool matched;
	double matched_at;
	int mismatch_best; /* smallest mismatch count seen, for diagnostics */

	/* last frame kept for the failure PNG */
	uint8_t *last;
	int last_w, last_h, last_stride;

	/* statistics window */
	bool stats_on;
	double stats_start, last_arrival, max_gap;
	long frames, partial, full;
	double grab_sum, grab_min, grab_max, area_sum;
} S = { .mismatch_best = -1, .grab_min = 1e9 };

static void on_mode(const struct vdisplay_mode *m, void *u)
{
	(void)u;
	LOG("mode_changed %dx%d@%d", m->width, m->height, m->refresh_hz);
}

static void prepare_reference(int w, int h)
{
	if (S.ref && S.ref_w == w && S.ref_h == h) return;
	free(S.ref);
	S.ref = malloc((size_t)w * h * 4);
	S.ref_w = w;
	S.ref_h = h;
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) S.ref[(size_t)y * w + x] = pattern_pixel(x, y, w, h);
}

/* Number of pixels (alpha ignored) that differ from the pattern; the first one is reported through x/y. */
static long count_mismatches(const struct vdisplay_frame *f, int *fx, int *fy)
{
	long bad = 0;
	for (int y = 0; y < f->height; y++) {
		const uint32_t *row = (const uint32_t *)(f->data + (size_t)f->stride * y);
		const uint32_t *ref = S.ref + (size_t)y * f->width;
		for (int x = 0; x < f->width; x++) {
			if ((row[x] & 0x00ffffffu) != ref[x]) {
				if (!bad) { *fx = x; *fy = y; }
				bad++;
			}
		}
	}
	return bad;
}

static void keep_last(const struct vdisplay_frame *f)
{
	size_t n = (size_t)f->stride * f->height;
	if (S.last_stride != f->stride || S.last_h != f->height) {
		free(S.last);
		S.last = malloc(n);
	}
	if (S.last) memcpy(S.last, f->data, n);
	S.last_w = f->width;
	S.last_h = f->height;
	S.last_stride = f->stride;
}

static void on_frame(const struct vdisplay_frame *f, void *u)
{
	(void)u;
	double t = now();

	if (S.verbose) {
		LOG("frame #%llu grab=%.2f ms rects=%d", (unsigned long long)f->seq, f->grab_ms, f->rect_count);
		for (int i = 0; i < f->rect_count && i < 3; i++)
			printf("         rect [%d,%d-%d,%d]\n", f->rects[i].x1, f->rects[i].y1, f->rects[i].x2, f->rects[i].y2);
	}

	if (S.verify && !S.matched) {
		prepare_reference(f->width, f->height);
		int fx = -1, fy = -1;
		long bad = count_mismatches(f, &fx, &fy);
		if (S.mismatch_best < 0 || bad < S.mismatch_best) S.mismatch_best = (int)bad;
		if (bad == 0) {
			S.matched = true;
			S.matched_at = t;
			LOG("frame #%llu matches the pattern exactly (%dx%d)", (unsigned long long)f->seq, f->width, f->height);
			if (S.png_path) {
				if (png_write_bgra(S.png_path, f->data, f->width, f->height, f->stride) == 0)
					LOG("wrote %s", S.png_path);
				else
					LOG("could not write %s", S.png_path);
			}
		}
	}
	if (!S.matched) keep_last(f);

	if (S.stats_on) {
		if (S.frames > 0 && t - S.last_arrival > S.max_gap) S.max_gap = t - S.last_arrival;
		S.last_arrival = t;
		S.frames++;
		S.grab_sum += f->grab_ms;
		if (f->grab_ms < S.grab_min) S.grab_min = f->grab_ms;
		if (f->grab_ms > S.grab_max) S.grab_max = f->grab_ms;
		double area = 0;
		for (int i = 0; i < f->rect_count; i++)
			area += (double)(f->rects[i].x2 - f->rects[i].x1) * (f->rects[i].y2 - f->rects[i].y1);
		area /= (double)f->width * f->height;
		S.area_sum += area;
		if (area >= 0.999) S.full++; else S.partial++;
	}
}

static void usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s [options]\n" EDID_CLI_USAGE
		"  -V          wait for a frame that equals the test pattern bit-exactly\n"
		"  -o FILE     write the matching (or, on failure, last) frame as PNG\n"
		"  -S SECS     capture for SECS seconds (after the match with -V) and print statistics\n"
		"  -T SECS     give up after this many seconds (default 30)\n"
		"  -d CARD     use /dev/dri/card<CARD> (default: first free evdi device)\n"
		"  -v          log every frame\n",
		argv0);
}

int main(int argc, char **argv)
{
	struct edid_params p;
	double timeout = 30;
	int card = -1, opt;

	edid_cli_defaults(&p);
	while ((opt = getopt(argc, argv, EDID_CLI_OPTS "Vo:S:T:d:vh")) != -1) {
		switch (opt) {
		case 'V': S.verify = true; break;
		case 'o': S.png_path = optarg; break;
		case 'S': S.stats_secs = atof(optarg); break;
		case 'T': timeout = atof(optarg); break;
		case 'd': card = atoi(optarg); break;
		case 'v': S.verbose = true; break;
		default:
			if (edid_cli_opt(&p, opt, optarg) != 1) { usage(argv[0]); return 2; }
		}
	}

	uint8_t edid[EDID_SIZE];
	struct edid_timing timing;
	if (edid_generate(&p, edid) != 0 || edid_compute_timing(p.width, p.height, p.refresh_mhz, &timing) != 0) {
		fprintf(stderr, "%s: parameters cannot be encoded in an EDID base block\n", argv[0]);
		return 1;
	}
	struct sigaction sa = { .sa_handler = on_signal };
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	t0 = now();

	if (card < 0) card = vdisplay_find_free_device();
	if (card < 0) {
		fprintf(stderr, "no free evdi device (create one: echo 1 > /sys/devices/evdi/add, as root)\n");
		return 3;
	}

	struct vdisplay_events ev = { .mode_changed = on_mode, .frame = on_frame };
	struct vdisplay *vd;
	int err = vdisplay_open(card, &ev, &vd);
	if (err) {
		fprintf(stderr, "cannot open /dev/dri/card%d: %s\n", card, strerror(-err));
		return 1;
	}
	if (vdisplay_connect(vd, edid, timing.hactive, timing.vactive, timing.pixel_clock_khz) != 0) {
		vdisplay_close(vd);
		return 1;
	}
	LOG("connected '%s' serial '%s' %ux%u@%.3f on card%d", p.name, p.serial_text, p.width, p.height, timing.refresh_hz, card);

	double deadline = now() + timeout;
	double stats_end = 0;
	if (!S.verify && S.stats_secs > 0) {
		S.stats_on = true;
		S.stats_start = now();
		stats_end = S.stats_start + S.stats_secs;
	}
	while (!stop && now() < deadline) {
		vdisplay_pump(vd, 50);
		if (S.matched && S.stats_secs > 0 && !S.stats_on) {
			S.stats_on = true;
			S.stats_start = now();
			stats_end = S.stats_start + S.stats_secs;
			LOG("collecting statistics for %.1f s", S.stats_secs);
		}
		if (S.verify && S.matched && S.stats_secs <= 0) break;
		if (stats_end > 0 && now() >= stats_end) break;
	}

	int rc = 0;
	if (S.verify && !S.matched) {
		LOG("FAIL: no frame matched the pattern within %.0f s (best frame still differed in %d pixels)", timeout,
		    S.mismatch_best);
		if (S.png_path && S.last && png_write_bgra(S.png_path, S.last, S.last_w, S.last_h, S.last_stride) == 0)
			LOG("wrote last frame to %s", S.png_path);
		rc = 1;
	} else if (S.verify) {
		LOG("PASS: pattern round-trip is bit-exact (first match %.3f s after connect)", S.matched_at - t0);
	}
	if (S.stats_on && S.frames > 0) {
		double span = S.last_arrival - S.stats_start;
		printf("STATS frames=%ld span=%.2fs fps=%.1f max_gap_ms=%.1f grab_ms_min=%.2f avg=%.2f max=%.2f "
		       "partial=%ld full=%ld avg_area=%.1f%%\n",
		       S.frames, span, S.frames / span, S.max_gap * 1000, S.grab_min, S.grab_sum / S.frames, S.grab_max,
		       S.partial, S.full, S.area_sum / S.frames * 100);
	} else if (S.stats_on) {
		printf("STATS frames=0\n");
	}

	vdisplay_close(vd);
	return rc;
}
