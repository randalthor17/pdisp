/* Virtual display: a thin owner of one evdi device.
 *
 * The device itself (/dev/dri/cardN, created through /sys/devices/evdi/add) must already exist: creating it
 * needs root, so a setup step does that, not this daemon. connect() presents a monitor with the given EDID to the
 * compositor; disconnect() removes it again. Both can be repeated on the same device, and the output keeps its
 * identity as long as the EDID make/model/serial are the same.
 */
#ifndef PDISP_VDISPLAY_H
#define PDISP_VDISPLAY_H

#include <stdbool.h>
#include <stdint.h>

#include "edid.h"

struct vdisplay;

struct vdisplay_mode {
	int width;
	int height;
	int refresh_hz;
};

struct vdisplay_events {
	/* the compositor set a mode on the output (this is the signal that the monitor was accepted) */
	void (*mode_changed)(const struct vdisplay_mode *mode, void *user);
	/* the output was blanked or woken (DPMS), or its CRTC was enabled or disabled */
	void (*dpms)(int mode, void *user);
	void (*crtc_state)(int state, void *user);
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

/* A file descriptor that becomes readable when vdisplay_dispatch() has work to do. */
int vdisplay_event_fd(const struct vdisplay *vd);
void vdisplay_dispatch(struct vdisplay *vd);

/* Disconnects if needed and releases the device. */
void vdisplay_close(struct vdisplay *vd);

#endif
