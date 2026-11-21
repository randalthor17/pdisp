/* pdisp-vdisplay: connect an evdi virtual monitor with a generated EDID, optionally cycling it.
 * Used for the M1 pass check (see tests/m1_niri_check.sh). Needs an existing evdi device. */
#include "edid_cli.h"
#include "vdisplay.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stop;
static void on_signal(int sig) { (void)sig; stop = 1; }

static double t0;
static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}
#define LOG(...) do { printf("[%7.3f] ", now() - t0); printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while (0)

static void on_mode(const struct vdisplay_mode *m, void *u) { (void)u; LOG("mode_changed %dx%d@%d", m->width, m->height, m->refresh_hz); }
static void on_dpms(int m, void *u) { (void)u; LOG("dpms %d", m); }
static void on_crtc(int s, void *u) { (void)u; LOG("crtc_state %d", s); }

/* Run the event loop for `secs` seconds, or until a signal arrives. */
static void run_for(struct vdisplay *vd, double secs)
{
	double end = now() + secs;
	while (!stop && now() < end) {
		int ms = (int)((end - now()) * 1000);
		if (ms > 200) ms = 200;
		if (ms < 0) ms = 0;
		vdisplay_pump(vd, ms);
	}
}

static void usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s [options]\n" EDID_CLI_USAGE
		"  -u SECS     seconds to stay connected per cycle (default: until SIGINT/SIGTERM)\n"
		"  -c CYCLES   connect/disconnect this many times (default 1)\n"
		"  -g SECS     seconds to stay disconnected between cycles (default 3)\n"
		"  -d CARD     use /dev/dri/card<CARD> (default: first free evdi device)\n",
		argv0);
}

int main(int argc, char **argv)
{
	struct edid_params p;
	double uptime = -1, gap = 3;
	int cycles = 1, card = -1, opt;

	edid_cli_defaults(&p);
	while ((opt = getopt(argc, argv, EDID_CLI_OPTS "u:c:g:d:h")) != -1) {
		switch (opt) {
		case 'u': uptime = atof(optarg); break;
		case 'c': cycles = atoi(optarg); break;
		case 'g': gap = atof(optarg); break;
		case 'd': card = atoi(optarg); break;
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
		return 3; /* distinct code so tests can skip instead of fail */
	}

	struct vdisplay_events ev = { .mode_changed = on_mode, .dpms = on_dpms, .crtc_state = on_crtc };
	struct vdisplay *vd;
	int err = vdisplay_open(card, &ev, &vd);
	if (err) {
		fprintf(stderr, "cannot open /dev/dri/card%d: %s\n", card, strerror(-err));
		return 1;
	}
	LOG("opened /dev/dri/card%d", card);

	for (int c = 0; !stop && c < cycles; c++) {
		if (c > 0) {
			LOG("disconnected for %.1fs", gap);
			run_for(vd, gap);
			if (stop) break;
		}
		err = vdisplay_connect(vd, edid, timing.hactive, timing.vactive, timing.pixel_clock_khz);
		if (err) {
			fprintf(stderr, "connect failed: %s\n", strerror(-err));
			vdisplay_close(vd);
			return 1;
		}
		LOG("connected '%s' serial '%s', requested %ux%u@%.3f", p.name, p.serial_text, p.width, p.height,
		    timing.refresh_hz);
		run_for(vd, uptime < 0 ? 1e9 : uptime);
		LOG("disconnect");
		vdisplay_disconnect(vd);
	}

	vdisplay_close(vd);
	LOG("closed");
	return 0;
}
