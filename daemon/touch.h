/* A virtual multitouch touchscreen on the PC (uinput), fed with the phone's touch events. The compositor sees an ordinary direct-touch
 * device (INPUT_PROP_DIRECT, protocol B multitouch) and does its own gesture recognition: neither the app nor the daemon does.
 * Which output the touches land on is the compositor's business (see docs/touch.md). */
#ifndef PDISP_TOUCH_H
#define PDISP_TOUCH_H

#include "proto.h"

#define TOUCH_MAX_SLOTS 10

struct touch_config {
	const char *name;     /* the device name the compositor sees (and that tests look for) */
	unsigned width, height; /* the phone's display in pixels: the coordinate range of the device */
	unsigned dpi;         /* so the compositor knows the physical size and can scale gestures */
	unsigned slots;       /* simultaneous touches, from the phone's HELLO (1 to TOUCH_MAX_SLOTS) */
};

struct touchdev;

/* Creates the device. Returns 0, or a negative errno (-ENOENT or -EACCES when /dev/uinput is missing or not writable). */
int touch_open(const struct touch_config *cfg, struct touchdev **out);
/* Removes the device; any finger still down is lifted first. */
void touch_close(struct touchdev *t);
/* Applies one event from the phone. Events for fingers beyond the slot count, hover and button events are ignored (0 is returned). */
int touch_event(struct touchdev *t, const struct proto_input_event *e);
/* Lifts every finger (after a lost event, or when the session ends). */
void touch_release_all(struct touchdev *t);
/* How many fingers are down right now. */
unsigned touch_fingers(const struct touchdev *t);

#endif
