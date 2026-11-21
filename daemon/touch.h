/* Virtual input devices on the PC (uinput), fed with the phone's events: a multitouch touchscreen for fingers and a pen tablet for the
 * stylus. The compositor sees ordinary direct-input devices and does its own focus, gestures and scrolling: neither the app nor the
 * daemon interprets gestures. Which output a device lands on is the compositor's business (see docs/touch.md).
 *
 * Both devices can also be built on a plain callback instead of /dev/uinput (touch_open_sink, pen_open_sink) so that their event
 * sequences can be tested without privileges.
 */
#ifndef PDISP_TOUCH_H
#define PDISP_TOUCH_H

#include <stdint.h>

#include "proto.h"

#define TOUCH_MAX_SLOTS 10

/* Where the devices send their evdev events: a (type, code, value) triple; a SYN_REPORT ends each frame. */
typedef int (*input_emit_fn)(void *user, uint16_t type, uint16_t code, int32_t value);

/* ---- touchscreen (fingers) */

struct touch_config {
	const char *name;     /* the device name the compositor sees (and that tests look for) */
	unsigned width, height; /* the phone's display in pixels: the coordinate range of the device */
	unsigned dpi;         /* so the compositor knows the physical size and can scale gestures */
	unsigned slots;       /* simultaneous touches, from the phone's HELLO (1 to TOUCH_MAX_SLOTS) */
};

struct touchdev;

/* Creates the device. Returns 0, or a negative errno (-ENOENT or -EACCES when /dev/uinput is missing or not writable). */
int touch_open(const struct touch_config *cfg, struct touchdev **out);
int touch_open_sink(const struct touch_config *cfg, input_emit_fn emit, void *user, struct touchdev **out);
/* Removes the device; any finger still down is lifted first. */
void touch_close(struct touchdev *t);
/* Applies one finger event from the phone. Events for other tools (the stylus), for fingers beyond the slot count and hover/button events
 * are ignored (0 is returned). */
int touch_event(struct touchdev *t, const struct proto_input_event *e);
/* Lifts every finger (after a lost event, or when the session ends). */
void touch_release_all(struct touchdev *t);
unsigned touch_fingers(const struct touchdev *t);

/* ---- pen tablet (stylus and eraser)
 *
 * Protocol events with tool STYLUS or ERASER:
 *   hover  the pen is near the screen but not touching; `pressure` carries the hover distance (0 close .. 65535 far)
 *   down   the tip touches (pressure = tip pressure); move: the pen moved (touching or hovering); up: the tip lifted, the pen is still near
 *   cancel the pen left the screen's proximity (and the tip lifts if it was down)
 *   button `buttons` bit 0 = barrel button, bit 1 = second button; every event also carries the current button state
 * x and y are fractions of the phone's display (0..65535); tilt_x and tilt_y are degrees. */

struct pen_config {
	const char *name;
	unsigned width, height, dpi;
};

struct pendev;

int pen_open(const struct pen_config *cfg, struct pendev **out);
int pen_open_sink(const struct pen_config *cfg, input_emit_fn emit, void *user, struct pendev **out);
void pen_close(struct pendev *p);
int pen_event(struct pendev *p, const struct proto_input_event *e);
void pen_release_all(struct pendev *p); /* the pen leaves: tip up, proximity out, buttons released */
int pen_in_proximity(const struct pendev *p);

#endif
