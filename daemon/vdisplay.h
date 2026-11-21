/* Virtual display: a thin owner of one evdi device.
 *
 * The device itself (/dev/dri/cardN, created through /sys/devices/evdi/add) must already exist: creating it
 * needs root, so a setup step does that, not this daemon. connect() presents a monitor with the given EDID to the
 * compositor; disconnect() removes it again. Both can be repeated on the same device, and the output keeps its
 * identity as long as the EDID make/model/serial are the same.
 *
 * Frames: if a frame handler is set, the library registers a buffer for each mode the compositor sets and
 * delivers every update through the handler. Drive everything from vdisplay_pump().
 */
#ifndef PDISP_VDISPLAY_H
#define PDISP_VDISPLAY_H

#include <stdbool.h>
#include <stdint.h>

#include "edid.h"

#define VDISPLAY_MAX_RECTS 16

struct vdisplay;

struct vdisplay_mode {
	int width;
	int height;
	int refresh_hz;
};

struct vdisplay_rect {
	int x1, y1, x2, y2; /* x2/y2 exclusive */
};

/* One screen update. `data` is BGRA (32 bits per pixel, B first) and is only valid during the callback. */
struct vdisplay_frame {
	const uint8_t *data;
	int width;
	int height;
	int stride; /* bytes per row */
	struct vdisplay_rect rects[VDISPLAY_MAX_RECTS]; /* changed regions; evdi reports one bounding box per update */
	int rect_count;
	uint64_t seq;       /* 1-based count of frames delivered since open */
	uint64_t grab_ns;   /* CLOCK_MONOTONIC at the start of the grab */
	double grab_ms;     /* duration of the grab */
};

struct vdisplay_events {
	/* the compositor set a mode on the output (this is the signal that the monitor was accepted) */
	void (*mode_changed)(const struct vdisplay_mode *mode, void *user);
	/* the output was blanked or woken (DPMS), or its CRTC was enabled or disabled */
	void (*dpms)(int mode, void *user);
	void (*crtc_state)(int state, void *user);
	/* a screen update; setting this enables frame capture */
	void (*frame)(const struct vdisplay_frame *frame, void *user);
	void *user;
};

/* Number of the first free evdi card (/dev/dri/cardN), or -ENODEV if there is none. */
int vdisplay_find_free_device(void);

/* Open evdi card `card` (from vdisplay_find_free_device). Returns 0 and a handle, or -errno. */
int vdisplay_open(int card, const struct vdisplay_events *events, struct vdisplay **out);

/* Present a monitor described by `edid` (and the mode it was generated for). Fails if already connected. */
int vdisplay_connect(struct vdisplay *vd, const uint8_t edid[EDID_SIZE], unsigned width, unsigned height,
		     unsigned pixel_clock_khz);
void vdisplay_disconnect(struct vdisplay *vd);
bool vdisplay_is_connected(const struct vdisplay *vd);

/* Run the event loop once: deliver every update that is ready, then wait up to `timeout_ms` for more events and
 * dispatch them. Returns early when events were handled. Call it repeatedly. */
void vdisplay_pump(struct vdisplay *vd, int timeout_ms);

/* Disconnects if needed and releases the device. */
void vdisplay_close(struct vdisplay *vd);

#endif
