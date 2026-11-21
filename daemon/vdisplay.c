#include "vdisplay.h"

#include <errno.h>
#include <evdi_lib.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CARDS 32

struct vdisplay {
	evdi_handle handle;
	bool connected;
	struct vdisplay_events events;
};

int vdisplay_find_free_device(void)
{
	for (int i = 0; i < MAX_CARDS; i++)
		if (evdi_check_device(i) == AVAILABLE) return i;
	return -ENODEV;
}

static void on_mode_changed(struct evdi_mode m, void *user)
{
	struct vdisplay *vd = user;
	if (!vd->events.mode_changed) return;
	struct vdisplay_mode mode = { .width = m.width, .height = m.height, .refresh_hz = m.refresh_rate };
	vd->events.mode_changed(&mode, vd->events.user);
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

/* Frames are M2. Until then these updates are not requested, so nothing else needs handling. */

int vdisplay_open(int card, const struct vdisplay_events *events, struct vdisplay **out)
{
	struct vdisplay *vd = calloc(1, sizeof *vd);
	if (!vd) return -ENOMEM;
	if (events) vd->events = *events;

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
}

bool vdisplay_is_connected(const struct vdisplay *vd)
{
	return vd->connected;
}

int vdisplay_event_fd(const struct vdisplay *vd)
{
	return evdi_get_event_ready(vd->handle);
}

void vdisplay_dispatch(struct vdisplay *vd)
{
	struct evdi_event_context ctx;
	memset(&ctx, 0, sizeof ctx);
	ctx.mode_changed_handler = on_mode_changed;
	ctx.dpms_handler = on_dpms;
	ctx.crtc_state_handler = on_crtc_state;
	ctx.user_data = vd;
	evdi_handle_events(vd->handle, &ctx);
}

void vdisplay_close(struct vdisplay *vd)
{
	if (!vd) return;
	vdisplay_disconnect(vd);
	evdi_close(vd->handle);
	free(vd);
}
