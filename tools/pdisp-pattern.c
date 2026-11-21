/* pdisp-pattern: fullscreen the deterministic test pattern (tools/pattern.h) on a named Wayland output.
 *
 * The buffer is exactly the output's pixel size and is attached unscaled, so (with the output at scale 1)
 * every pixel reaches the scanout unchanged. With -a a bar sweeps across the pattern on every frame callback,
 * which gives a steady stream of damage for capture and latency tests.
 */
#define _GNU_SOURCE
#include "pattern.h"
#include "xdg-shell-client-protocol.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#define MAX_OUTPUTS 16
#define NBUF 3
#define BAR_WIDTH 64
#define BAR_STEP 16

struct output {
	struct wl_output *wl;
	char name[64];
	int width, height; /* pixels of the current mode */
};

struct buffer {
	struct wl_buffer *wl;
	uint32_t *pixels;
	bool busy;
};

static struct {
	struct wl_display *display;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct xdg_wm_base *wm;
	struct output outputs[MAX_OUTPUTS];
	int n_outputs;

	struct wl_surface *surface;
	struct xdg_toplevel *toplevel;
	struct output *target;
	int width, height;
	bool configured, animate;
	uint32_t *base; /* the pattern, computed once */
	struct buffer buf[NBUF];
	unsigned frame_no;
	int bar_x; /* position of the bar in the last committed frame, -1 if none */
	long committed;
} app;

static volatile sig_atomic_t stop;
static void on_signal(int sig) { (void)sig; stop = 1; }

/* ---- wl_output (v4: name) */
static void out_geometry(void *d, struct wl_output *o, int32_t x, int32_t y, int32_t pw, int32_t ph, int32_t sp,
			 const char *make, const char *model, int32_t tr)
{ (void)d; (void)o; (void)x; (void)y; (void)pw; (void)ph; (void)sp; (void)make; (void)model; (void)tr; }
static void out_mode(void *d, struct wl_output *o, uint32_t flags, int32_t w, int32_t h, int32_t r)
{
	(void)o; (void)r;
	struct output *out = d;
	if (flags & WL_OUTPUT_MODE_CURRENT) { out->width = w; out->height = h; }
}
static void out_done(void *d, struct wl_output *o) { (void)d; (void)o; }
static void out_scale(void *d, struct wl_output *o, int32_t f) { (void)d; (void)o; (void)f; }
static void out_name(void *d, struct wl_output *o, const char *name)
{
	(void)o;
	struct output *out = d;
	snprintf(out->name, sizeof out->name, "%s", name);
}
static void out_description(void *d, struct wl_output *o, const char *desc) { (void)d; (void)o; (void)desc; }
static const struct wl_output_listener output_listener = {
	out_geometry, out_mode, out_done, out_scale, out_name, out_description,
};

/* ---- registry */
static void wm_ping(void *d, struct xdg_wm_base *wm, uint32_t serial) { (void)d; xdg_wm_base_pong(wm, serial); }
static const struct xdg_wm_base_listener wm_listener = { wm_ping };

static void reg_global(void *d, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t version)
{
	(void)d;
	if (!strcmp(iface, wl_compositor_interface.name)) {
		app.compositor = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
	} else if (!strcmp(iface, wl_shm_interface.name)) {
		app.shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
	} else if (!strcmp(iface, xdg_wm_base_interface.name)) {
		app.wm = wl_registry_bind(reg, name, &xdg_wm_base_interface, 1);
		xdg_wm_base_add_listener(app.wm, &wm_listener, NULL);
	} else if (!strcmp(iface, wl_output_interface.name) && app.n_outputs < MAX_OUTPUTS) {
		struct output *o = &app.outputs[app.n_outputs++];
		uint32_t v = version < 4 ? version : 4;
		o->wl = wl_registry_bind(reg, name, &wl_output_interface, v);
		wl_output_add_listener(o->wl, &output_listener, o);
	}
}
static void reg_remove(void *d, struct wl_registry *reg, uint32_t name) { (void)d; (void)reg; (void)name; }
static const struct wl_registry_listener registry_listener = { reg_global, reg_remove };

/* ---- buffers */
static void buffer_release(void *d, struct wl_buffer *b)
{
	(void)b;
	((struct buffer *)d)->busy = false;
}
static const struct wl_buffer_listener buffer_listener = { buffer_release };

static int create_buffers(void)
{
	size_t stride = (size_t)app.width * 4, size = stride * app.height;
	int fd = memfd_create("pdisp-pattern", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, (off_t)size * NBUF) < 0) return -1;
	void *map = mmap(NULL, size * NBUF, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED) return -1;
	struct wl_shm_pool *pool = wl_shm_create_pool(app.shm, fd, (int32_t)(size * NBUF));
	for (int i = 0; i < NBUF; i++) {
		app.buf[i].pixels = (uint32_t *)((char *)map + size * i);
		app.buf[i].wl = wl_shm_pool_create_buffer(pool, (int32_t)(size * i), app.width, app.height, (int32_t)stride,
							  WL_SHM_FORMAT_XRGB8888);
		wl_buffer_add_listener(app.buf[i].wl, &buffer_listener, &app.buf[i]);
	}
	wl_shm_pool_destroy(pool);
	close(fd);

	app.base = malloc(size);
	if (!app.base) return -1;
	for (int y = 0; y < app.height; y++)
		for (int x = 0; x < app.width; x++)
			app.base[(size_t)y * app.width + x] = pattern_pixel(x, y, app.width, app.height);
	return 0;
}

static void draw_and_commit(void);

static void frame_done(void *d, struct wl_callback *cb, uint32_t time)
{
	(void)d; (void)time;
	wl_callback_destroy(cb);
	draw_and_commit();
}
static const struct wl_callback_listener frame_listener = { frame_done };

static void draw_and_commit(void)
{
	struct buffer *b = NULL;
	for (int i = 0; i < NBUF; i++)
		if (!app.buf[i].busy) { b = &app.buf[i]; break; }
	if (!b) { /* compositor still holds every buffer: try again on the next frame */
		wl_callback_add_listener(wl_surface_frame(app.surface), &frame_listener, NULL);
		wl_surface_commit(app.surface);
		return;
	}

	size_t n = (size_t)app.width * app.height;
	memcpy(b->pixels, app.base, n * 4);

	/* Damage only what changed since the previous commit: the whole surface for the first frame, afterwards
	 * the bar's old and new positions. (Every buffer is rewritten completely, so it is correct either way.) */
	int new_x = -1;
	if (app.animate) {
		new_x = (int)((app.frame_no * BAR_STEP) % (unsigned)(app.width - BAR_WIDTH));
		for (int y = 0; y < app.height; y++)
			for (int x = new_x; x < new_x + BAR_WIDTH; x++) b->pixels[(size_t)y * app.width + x] = 0x00ff00ffu;
	}

	b->busy = true;
	wl_surface_attach(app.surface, b->wl, 0, 0);
	if (!app.animate || app.frame_no == 0) {
		wl_surface_damage_buffer(app.surface, 0, 0, app.width, app.height);
	} else {
		if (app.bar_x >= 0) wl_surface_damage_buffer(app.surface, app.bar_x, 0, BAR_WIDTH, app.height);
		wl_surface_damage_buffer(app.surface, new_x, 0, BAR_WIDTH, app.height);
	}
	app.bar_x = new_x;
	app.frame_no++;
	if (app.animate) wl_callback_add_listener(wl_surface_frame(app.surface), &frame_listener, NULL);
	wl_surface_commit(app.surface);
	app.committed++;
}

/* ---- xdg-shell */
static void xs_configure(void *d, struct xdg_surface *xs, uint32_t serial)
{
	(void)d;
	xdg_surface_ack_configure(xs, serial);
	app.configured = true; /* main() allocates buffers and draws the first frame once the size is checked */
}
static const struct xdg_surface_listener xs_listener = { xs_configure };

static void tl_configure(void *d, struct xdg_toplevel *tl, int32_t w, int32_t h, struct wl_array *states)
{
	(void)d; (void)tl; (void)states;
	if (!app.configured) {
		app.width = w > 0 ? w : app.target->width;
		app.height = h > 0 ? h : app.target->height;
	}
}
static void tl_close(void *d, struct xdg_toplevel *tl) { (void)d; (void)tl; stop = 1; }
static void tl_bounds(void *d, struct xdg_toplevel *tl, int32_t w, int32_t h) { (void)d; (void)tl; (void)w; (void)h; }
static void tl_caps(void *d, struct xdg_toplevel *tl, struct wl_array *c) { (void)d; (void)tl; (void)c; }
static const struct xdg_toplevel_listener tl_listener = { tl_configure, tl_close, tl_bounds, tl_caps };

static void usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s -o OUTPUT [-t SECS] [-a]\n"
		"  -o OUTPUT  connector name of the output to fullscreen on (e.g. DVI-I-1)\n"
		"  -t SECS    exit after this many seconds (default: until SIGINT/SIGTERM)\n"
		"  -a         animate: sweep a bar across the pattern on every frame\n",
		argv0);
}

int main(int argc, char **argv)
{
	const char *want = NULL;
	double secs = -1;
	int opt;
	while ((opt = getopt(argc, argv, "o:t:ah")) != -1) {
		switch (opt) {
		case 'o': want = optarg; break;
		case 't': secs = atof(optarg); break;
		case 'a': app.animate = true; break;
		default: usage(argv[0]); return 2;
		}
	}
	if (!want) { usage(argv[0]); return 2; }

	struct sigaction sa = { .sa_handler = on_signal };
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	app.display = wl_display_connect(NULL);
	if (!app.display) { fprintf(stderr, "cannot connect to the Wayland display\n"); return 1; }
	struct wl_registry *reg = wl_display_get_registry(app.display);
	wl_registry_add_listener(reg, &registry_listener, NULL);
	wl_display_roundtrip(app.display); /* globals */
	wl_display_roundtrip(app.display); /* output names and modes */

	if (!app.compositor || !app.shm || !app.wm) { fprintf(stderr, "compositor lacks wl_compositor/wl_shm/xdg_wm_base\n"); return 1; }
	for (int i = 0; i < app.n_outputs; i++)
		if (!strcmp(app.outputs[i].name, want)) app.target = &app.outputs[i];
	if (!app.target) {
		fprintf(stderr, "no output named %s (have:", want);
		for (int i = 0; i < app.n_outputs; i++) fprintf(stderr, " %s", app.outputs[i].name);
		fprintf(stderr, ")\n");
		return 1;
	}

	app.surface = wl_compositor_create_surface(app.compositor);
	struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(app.wm, app.surface);
	xdg_surface_add_listener(xs, &xs_listener, NULL);
	app.toplevel = xdg_surface_get_toplevel(xs);
	xdg_toplevel_add_listener(app.toplevel, &tl_listener, NULL);
	xdg_toplevel_set_title(app.toplevel, "pdisp-pattern");
	xdg_toplevel_set_app_id(app.toplevel, "pdisp-pattern");
	xdg_toplevel_set_fullscreen(app.toplevel, app.target->wl);
	wl_surface_commit(app.surface);

	/* wait for the first configure, then check the size before allocating */
	while (!app.configured && !stop && wl_display_dispatch(app.display) >= 0) {}
	if (stop) return 1;
	if (app.width != app.target->width || app.height != app.target->height) {
		fprintf(stderr,
			"fullscreen size %dx%d differs from the output's %dx%d pixels: set the output scale to 1 "
			"(niri msg output %s scale 1)\n",
			app.width, app.height, app.target->width, app.target->height, want);
		return 2;
	}
	if (create_buffers() != 0) { fprintf(stderr, "cannot create shm buffers\n"); return 1; }
	draw_and_commit();
	printf("pattern on %s: %dx%d%s\n", want, app.width, app.height, app.animate ? " (animated)" : "");
	fflush(stdout);

	struct timespec t0;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	int fd = wl_display_get_fd(app.display);
	while (!stop) {
		while (wl_display_prepare_read(app.display) != 0) wl_display_dispatch_pending(app.display);
		wl_display_flush(app.display);
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		if (poll(&pfd, 1, 100) > 0) wl_display_read_events(app.display);
		else wl_display_cancel_read(app.display);
		wl_display_dispatch_pending(app.display);

		struct timespec t;
		clock_gettime(CLOCK_MONOTONIC, &t);
		if (secs >= 0 && (t.tv_sec - t0.tv_sec) + (t.tv_nsec - t0.tv_nsec) / 1e9 >= secs) break;
	}
	printf("committed %ld buffers\n", app.committed);
	return 0;
}
