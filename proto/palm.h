/* Palm rejection for the phone's touches, applied before they are sent (so a resting palm costs no bandwidth and the PC never sees it).
 *
 * Rules, in order of importance:
 *  1. the pen wins: when the stylus comes near, every finger already down is cancelled (it is probably the hand holding the pen), and new
 *     fingers are refused while the pen is near and for `pen_grace_us` after it left;
 *  2. a contact the system itself calls a palm (TOOL_TYPE_PALM) or has cancelled (FLAG_CANCELED) is refused, and cancelled if it was already
 *     forwarded;
 *  3. a contact larger than `palm_size_px` is a palm, and so is a finger that grows past it while it moves.
 * A refused contact stays refused until it lifts, even if the pen goes away or it shrinks: a palm does not turn into a finger.
 * The stylus's own events always pass through unchanged.
 */
#ifndef PDISP_PALM_H
#define PDISP_PALM_H

#include <stdint.h>

#include "proto.h"

#define PALM_MAX_POINTERS 32
#define PALM_MAX_OUT (PALM_MAX_POINTERS + 1)

enum { PALM_FLAG_PALM_TOOL = 1, PALM_FLAG_CANCELED = 2 };

struct palm_config {
	int enabled;
	unsigned palm_size_px;  /* a contact whose major axis is longer than this is a palm; 0 = no size rule */
	uint64_t pen_grace_us;  /* fingers stay refused this long after the pen left (default 400 ms) */
	uint64_t pen_timeout_us; /* the pen counts as gone after this long without any event, in case its leave was missed (default 600 ms) */
};

struct palm_filter {
	struct palm_config cfg;
	uint8_t forwarded[PALM_MAX_POINTERS];
	uint8_t rejected[PALM_MAX_POINTERS];
	int pen_near;
	uint64_t pen_seen_us, pen_left_us;
	uint64_t rejected_contacts, cancelled_fingers;
};

/* One raw contact event from the screen, with what only the screen knows. */
struct palm_input {
	struct proto_input_event ev;
	unsigned size_px; /* the contact's major axis in pixels, 0 if unknown */
	unsigned flags;
	uint64_t now_us;
};

void palm_init(struct palm_filter *f, const struct palm_config *cfg);
/* Filters one event. Fills `out` with the events to send, in order (the event itself, and/or cancels for fingers it made suspect),
 * and returns how many. */
unsigned palm_filter(struct palm_filter *f, const struct palm_input *in, struct proto_input_event out[PALM_MAX_OUT]);

#endif
