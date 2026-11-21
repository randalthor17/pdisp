#include "vdisplay.h"

#include <errno.h>
#include <evdi_lib.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_CARDS 32
#define BUFFER_ID 1
/* bound the synchronous grab loop so a compositor that always has a frame ready cannot starve event dispatch */
#define MAX_SYNC_GRABS 8

struct vdisplay {
	evdi_handle handle;
	bool connected;
	struct vdisplay_events events;

	/* frame capture */
	bool want_frames;
	bool have_buffer;
	bool armed; /* an update request is pending in the kernel */
	uint8_t *pixels;
	struct evdi_buffer buffer;
	struct evdi_rect rects[VDISPLAY_MAX_RECTS];
	uint64_t seq;
};

static uint64_t mono_ns(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

int vdisplay_find_free_device(void)
{
	for (int i = 0; i < MAX_CARDS; i++)
		if (evdi_check_device(i) == AVAILABLE) return i;
	return -ENODEV;
}

static void drop_buffer(struct vdisplay *vd)
{
	if (!vd->have_buffer) return;
	evdi_unregister_buffer(vd->handle, BUFFER_ID);
	free(vd->pixels);
	vd->pixels = NULL;
	vd->have_buffer = false;
	vd->armed = false;
}

static void grab_and_deliver(struct vdisplay *vd)
{
	int n = VDISPLAY_MAX_RECTS;
	uint64_t t0 = mono_ns();
	evdi_grab_pixels(vd->handle, vd->rects, &n);
	uint64_t t1 = mono_ns();
	vd->armed = false;

	struct vdisplay_frame f;
	memset(&f, 0, sizeof f);
	f.data = vd->pixels;
	f.width = vd->buffer.width;
	f.height = vd->buffer.height;
	f.stride = vd->buffer.stride;
	f.rect_count = n > VDISPLAY_MAX_RECTS ? VDISPLAY_MAX_RECTS : n;
	for (int i = 0; i < f.rect_count; i++)
		f.rects[i] = (struct vdisplay_rect){ vd->rects[i].x1, vd->rects[i].y1, vd->rects[i].x2, vd->rects[i].y2 };
	f.seq = ++vd->seq;
	f.grab_ns = t0;
	f.grab_ms = (t1 - t0) / 1e6;
	if (vd->events.frame) vd->events.frame(&f, vd->events.user);
}

static void on_mode_changed(struct evdi_mode m, void *user)
{
	struct vdisplay *vd = user;
	if (vd->want_frames) {
		/* a new mode needs a new buffer of the right size */
		drop_buffer(vd);
		vd->buffer = (struct evdi_buffer){ .id = BUFFER_ID, .width = m.width, .height = m.height,
						   .stride = m.width * 4, .rects = vd->rects,
						   .rect_count = VDISPLAY_MAX_RECTS };
		vd->pixels = calloc((size_t)vd->buffer.stride, (size_t)m.height);
		if (vd->pixels) {
			vd->buffer.buffer = vd->pixels;
			evdi_register_buffer(vd->handle, vd->buffer);
			vd->have_buffer = true;
		}
	}
	if (vd->events.mode_changed) {
		struct vdisplay_mode mode = { .width = m.width, .height = m.height, .refresh_hz = m.refresh_rate };
		vd->events.mode_changed(&mode, vd->events.user);
	}
}

static void on_update_ready(int buffer_id, void *user)
{
	struct vdisplay *vd = user;
	(void)buffer_id;
	if (vd->have_buffer) grab_and_deliver(vd);
}

static void on_dpms(int mode, void *user)
{
	struct vdisplay *vd = user;
	if (vd->events.dpms) vd->events.dpms(mode, vd->events.user);
}

static void on_crtc_state(int state, void *user)
{
	struct vdisplay *vd = user;
	if (vd->events.crtc_state) vd->events.crtc_state(state, vd->events.user);
}

int vdisplay_open(int card, const struct vdisplay_events *events, struct vdisplay **out)
{
	struct vdisplay *vd = calloc(1, sizeof *vd);
	if (!vd) return -ENOMEM;
	if (events) vd->events = *events;
	vd->want_frames = vd->events.frame != NULL;

	vd->handle = evdi_open(card);
	if (vd->handle == EVDI_INVALID_HANDLE) {
		free(vd);
		return -ENODEV;
	}
	*out = vd;
	return 0;
}

int vdisplay_connect(struct vdisplay *vd, const uint8_t edid[EDID_SIZE], unsigned width, unsigned height,
		     unsigned pixel_clock_khz)
{
	if (vd->connected) return -EALREADY;
	/* Limit the modes evdi derives from the EDID to ours: area and pixel rate no larger than the chosen timing. */
	uint32_t area = width * height;
	uint32_t pps = pixel_clock_khz * 1000u;
	evdi_connect2(vd->handle, edid, EDID_SIZE, area, pps);
	vd->connected = true;
	return 0;
}

void vdisplay_disconnect(struct vdisplay *vd)
{
	if (!vd->connected) return;
	evdi_disconnect(vd->handle);
	vd->connected = false;
	vd->armed = false; /* a pending request does not survive the disconnect */
}

bool vdisplay_is_connected(const struct vdisplay *vd)
{
	return vd->connected;
}

static void dispatch(struct vdisplay *vd)
{
	struct evdi_event_context ctx;
	memset(&ctx, 0, sizeof ctx);
	ctx.mode_changed_handler = on_mode_changed;
	ctx.update_ready_handler = on_update_ready;
	ctx.dpms_handler = on_dpms;
	ctx.crtc_state_handler = on_crtc_state;
	ctx.user_data = vd;
	evdi_handle_events(vd->handle, &ctx);
}

bool vdisplay_current_frame(const struct vdisplay *vd, struct vdisplay_frame *out)
{
	if (!vd->have_buffer || vd->seq == 0) return false;
	memset(out, 0, sizeof *out);
	out->data = vd->pixels;
	out->width = vd->buffer.width;
	out->height = vd->buffer.height;
	out->stride = vd->buffer.stride;
	out->seq = vd->seq;
	return true;
}

int vdisplay_event_fd(const struct vdisplay *vd)
{
	return evdi_get_event_ready(vd->handle);
}

void vdisplay_service(struct vdisplay *vd)
{
	vdisplay_pump(vd, 0);
}

void vdisplay_pump(struct vdisplay *vd, int timeout_ms)
{
	struct pollfd pfd = { .fd = evdi_get_event_ready(vd->handle), .events = POLLIN };

	/* An update that is already ready is returned by the request itself and raises no event, so grab it now.
	 * Waiting in poll() first would delay it by the whole timeout. */
	if (vd->want_frames && vd->connected && vd->have_buffer) {
		for (int i = 0; i < MAX_SYNC_GRABS && !vd->armed; i++) {
			if (!evdi_request_update(vd->handle, BUFFER_ID)) {
				vd->armed = true; /* not ready yet: an update_ready event will follow */
				break;
			}
			grab_and_deliver(vd);
		}
		if (!vd->armed) timeout_ms = 0; /* more frames may be waiting; do not block */
	}

	if (poll(&pfd, 1, timeout_ms) > 0) dispatch(vd);
}

void vdisplay_close(struct vdisplay *vd)
{
	if (!vd) return;
	vdisplay_disconnect(vd);
	drop_buffer(vd);
	evdi_close(vd->handle);
	free(vd);
}
