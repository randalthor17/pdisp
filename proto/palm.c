#include "palm.h"

#include <string.h>

void palm_init(struct palm_filter *f, const struct palm_config *cfg)
{
	memset(f, 0, sizeof *f);
	f->cfg = *cfg;
	if (!f->cfg.pen_grace_us) f->cfg.pen_grace_us = 400000;
	if (!f->cfg.pen_timeout_us) f->cfg.pen_timeout_us = 600000;
}

static unsigned cancel_finger(struct palm_filter *f, unsigned id, struct proto_input_event *out)
{
	if (!f->forwarded[id]) return 0;
	f->forwarded[id] = 0;
	f->cancelled_fingers++;
	memset(out, 0, sizeof *out);
	out->kind = PROTO_IN_CANCEL;
	out->pointer_id = (uint8_t)id;
	out->tool = PROTO_TOOL_FINGER;
	return 1;
}

static int pen_blocks(const struct palm_filter *f, uint64_t now)
{
	if (f->pen_near) return 1;
	return f->pen_left_us && now - f->pen_left_us < f->cfg.pen_grace_us;
}

unsigned palm_filter(struct palm_filter *f, const struct palm_input *in, struct proto_input_event out[PALM_MAX_OUT])
{
	unsigned n = 0;
	const struct proto_input_event *e = &in->ev;
	if (!f->cfg.enabled) { out[0] = *e; return 1; }

	/* a pen that stopped reporting without saying goodbye is gone */
	if (f->pen_near && in->now_us - f->pen_seen_us > f->cfg.pen_timeout_us) { f->pen_near = 0; f->pen_left_us = f->pen_seen_us; }

	if (e->tool == PROTO_TOOL_STYLUS || e->tool == PROTO_TOOL_ERASER) {
		if (e->kind == PROTO_IN_CANCEL) {
			if (f->pen_near) { f->pen_near = 0; f->pen_left_us = in->now_us; }
		} else {
			if (!f->pen_near) { /* the pen arrived: the fingers already down are the hand that holds it */
				f->pen_near = 1;
				for (unsigned id = 0; id < PALM_MAX_POINTERS; id++) {
					if (!f->forwarded[id]) continue;
					n += cancel_finger(f, id, &out[n]);
					f->rejected[id] = 1;
				}
			}
			f->pen_seen_us = in->now_us;
		}
		out[n++] = *e;
		return n;
	}

	unsigned id = e->pointer_id;
	if (id >= PALM_MAX_POINTERS) return 0;
	int palm_now = (in->flags & (PALM_FLAG_PALM_TOOL | PALM_FLAG_CANCELED)) || (f->cfg.palm_size_px && in->size_px > f->cfg.palm_size_px);

	switch (e->kind) {
	case PROTO_IN_DOWN:
		f->forwarded[id] = 0;
		f->rejected[id] = 0;
		if (palm_now || pen_blocks(f, in->now_us)) { f->rejected[id] = 1; f->rejected_contacts++; return 0; }
		f->forwarded[id] = 1;
		out[n++] = *e;
		return n;
	case PROTO_IN_MOVE:
		if (f->rejected[id] || !f->forwarded[id]) return 0;
		if (palm_now) { /* it was a finger, and became a palm */
			n += cancel_finger(f, id, &out[n]);
			f->rejected[id] = 1;
			f->rejected_contacts++;
			return n;
		}
		out[n++] = *e;
		return n;
	case PROTO_IN_UP:
		if (f->rejected[id]) { f->rejected[id] = 0; return 0; }
		if (!f->forwarded[id]) return 0;
		f->forwarded[id] = 0;
		if (in->flags & PALM_FLAG_CANCELED) { /* the system lifted it as a cancel: the finger never really finished */
			f->cancelled_fingers++;
			out[n] = *e;
			out[n].kind = PROTO_IN_CANCEL;
			return 1;
		}
		out[n++] = *e;
		return n;
	case PROTO_IN_CANCEL:
		f->rejected[id] = 0;
		if (!f->forwarded[id]) return 0;
		f->forwarded[id] = 0;
		out[n++] = *e;
		return n;
	default: /* hover and buttons of a finger do not exist */
		return 0;
	}
}
