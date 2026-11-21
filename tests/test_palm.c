/* Palm rejection: which contacts reach the PC, and which cancels are made up on the way. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "palm.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static uint64_t clock_us;
static struct proto_input_event out[PALM_MAX_OUT];

static unsigned feed_flags(struct palm_filter *f, uint8_t kind, uint8_t tool, uint8_t id, unsigned size, unsigned flags)
{
	struct palm_input in;
	memset(&in, 0, sizeof in);
	in.ev.kind = kind; in.ev.tool = tool; in.ev.pointer_id = id; in.ev.x = (uint16_t)(1000 + id); in.ev.y = 2000;
	in.size_px = size; in.flags = flags; in.now_us = clock_us;
	return palm_filter(f, &in, out);
}
#define FINGER(f, kind, id, size) feed_flags(f, kind, PROTO_TOOL_FINGER, id, size, 0)
#define PEN(f, kind) feed_flags(f, kind, PROTO_TOOL_STYLUS, 0, 0, 0)
static void ms(unsigned n) { clock_us += (uint64_t)n * 1000; }

static void fresh(struct palm_filter *f, unsigned palm_px)
{
	palm_init(f, &(struct palm_config){ .enabled = 1, .palm_size_px = palm_px });
	clock_us = 1000000;
}

int main(void)
{
	struct palm_filter f;

	/* a normal finger passes untouched */
	fresh(&f, 160);
	CHECK(FINGER(&f, PROTO_IN_DOWN, 0, 70) == 1 && out[0].kind == PROTO_IN_DOWN && out[0].x == 1000);
	CHECK(FINGER(&f, PROTO_IN_MOVE, 0, 75) == 1);
	CHECK(FINGER(&f, PROTO_IN_UP, 0, 0) == 1 && out[0].kind == PROTO_IN_UP);
	CHECK(FINGER(&f, PROTO_IN_DOWN, 1, 0) == 1); /* size unknown: a finger */
	FINGER(&f, PROTO_IN_UP, 1, 0);

	/* a palm lands: nothing is sent, not its moves, not its lift */
	fresh(&f, 160);
	CHECK(FINGER(&f, PROTO_IN_DOWN, 0, 300) == 0 && FINGER(&f, PROTO_IN_MOVE, 0, 300) == 0 && FINGER(&f, PROTO_IN_MOVE, 0, 100) == 0);
	CHECK(FINGER(&f, PROTO_IN_UP, 0, 0) == 0);
	CHECK(f.rejected_contacts == 1);
	CHECK(FINGER(&f, PROTO_IN_DOWN, 0, 70) == 1); /* the same pointer id, a finger this time */
	FINGER(&f, PROTO_IN_UP, 0, 0);

	/* a finger whose contact grows into a palm is cancelled once, and stays out */
	fresh(&f, 160);
	FINGER(&f, PROTO_IN_DOWN, 2, 80);
	CHECK(FINGER(&f, PROTO_IN_MOVE, 2, 120) == 1);
	unsigned n = FINGER(&f, PROTO_IN_MOVE, 2, 250);
	CHECK(n == 1 && out[0].kind == PROTO_IN_CANCEL && out[0].pointer_id == 2 && out[0].tool == PROTO_TOOL_FINGER);
	CHECK(FINGER(&f, PROTO_IN_MOVE, 2, 90) == 0 && FINGER(&f, PROTO_IN_UP, 2, 0) == 0); /* a palm does not turn back into a finger */

	/* the system's own verdicts: palm tool and cancelled */
	fresh(&f, 0);
	CHECK(feed_flags(&f, PROTO_IN_DOWN, PROTO_TOOL_FINGER, 0, 50, PALM_FLAG_PALM_TOOL) == 0);
	feed_flags(&f, PROTO_IN_UP, PROTO_TOOL_FINGER, 0, 0, 0);
	FINGER(&f, PROTO_IN_DOWN, 1, 50);
	n = feed_flags(&f, PROTO_IN_MOVE, PROTO_TOOL_FINGER, 1, 50, PALM_FLAG_CANCELED);
	CHECK(n == 1 && out[0].kind == PROTO_IN_CANCEL);
	FINGER(&f, PROTO_IN_DOWN, 2, 50);
	n = feed_flags(&f, PROTO_IN_UP, PROTO_TOOL_FINGER, 2, 50, PALM_FLAG_CANCELED); /* lifted as a cancel */
	CHECK(n == 1 && out[0].kind == PROTO_IN_CANCEL && out[0].pointer_id == 2);

	/* the pen wins: two fingers resting down are cancelled when it comes near; the pen's own events are untouched */
	fresh(&f, 160);
	FINGER(&f, PROTO_IN_DOWN, 0, 70);
	FINGER(&f, PROTO_IN_DOWN, 1, 70);
	ms(50);
	n = PEN(&f, PROTO_IN_HOVER);
	CHECK(n == 3 && out[0].kind == PROTO_IN_CANCEL && out[1].kind == PROTO_IN_CANCEL && out[2].kind == PROTO_IN_HOVER && out[2].tool == PROTO_TOOL_STYLUS);
	CHECK((out[0].pointer_id == 0 && out[1].pointer_id == 1) || (out[0].pointer_id == 1 && out[1].pointer_id == 0));
	CHECK(FINGER(&f, PROTO_IN_MOVE, 0, 70) == 0 && FINGER(&f, PROTO_IN_UP, 0, 0) == 0); /* those fingers are over */
	CHECK(PEN(&f, PROTO_IN_DOWN) == 1 && PEN(&f, PROTO_IN_MOVE) == 1 && PEN(&f, PROTO_IN_UP) == 1);
	ms(100);
	CHECK(FINGER(&f, PROTO_IN_DOWN, 3, 70) == 0); /* a new finger while the pen is near: a palm */
	FINGER(&f, PROTO_IN_UP, 3, 0);
	CHECK(PEN(&f, PROTO_IN_CANCEL) == 1 && out[0].kind == PROTO_IN_CANCEL && out[0].tool == PROTO_TOOL_STYLUS);
	ms(200);
	CHECK(FINGER(&f, PROTO_IN_DOWN, 3, 70) == 0); /* within the grace time after the pen left */
	FINGER(&f, PROTO_IN_UP, 3, 0);
	ms(300);
	CHECK(FINGER(&f, PROTO_IN_DOWN, 3, 70) == 1); /* later, fingers work again */
	FINGER(&f, PROTO_IN_UP, 3, 0);

	/* a rejected contact stays rejected after the pen is gone */
	fresh(&f, 160);
	PEN(&f, PROTO_IN_HOVER);
	CHECK(FINGER(&f, PROTO_IN_DOWN, 5, 70) == 0);
	PEN(&f, PROTO_IN_CANCEL);
	ms(2000);
	CHECK(FINGER(&f, PROTO_IN_MOVE, 5, 70) == 0 && FINGER(&f, PROTO_IN_UP, 5, 0) == 0);
	CHECK(FINGER(&f, PROTO_IN_DOWN, 5, 70) == 1);
	FINGER(&f, PROTO_IN_UP, 5, 0);

	/* a pen that went away without a leave event is eventually forgotten */
	fresh(&f, 160);
	PEN(&f, PROTO_IN_HOVER);
	ms(300);
	CHECK(FINGER(&f, PROTO_IN_DOWN, 0, 70) == 0);
	FINGER(&f, PROTO_IN_UP, 0, 0);
	ms(2000);
	CHECK(FINGER(&f, PROTO_IN_DOWN, 0, 70) == 1);
	FINGER(&f, PROTO_IN_UP, 0, 0);

	/* the eraser is a pen too */
	fresh(&f, 160);
	CHECK(feed_flags(&f, PROTO_IN_HOVER, PROTO_TOOL_ERASER, 0, 0, 0) == 1);
	CHECK(FINGER(&f, PROTO_IN_DOWN, 0, 70) == 0);

	/* cancels and ups of fingers that were never forwarded produce nothing; id out of range is ignored */
	fresh(&f, 160);
	CHECK(FINGER(&f, PROTO_IN_CANCEL, 4, 0) == 0 && FINGER(&f, PROTO_IN_UP, 4, 0) == 0 && FINGER(&f, PROTO_IN_MOVE, 4, 70) == 0);
	CHECK(FINGER(&f, PROTO_IN_DOWN, 40, 70) == 0);
	FINGER(&f, PROTO_IN_DOWN, 6, 70);
	CHECK(FINGER(&f, PROTO_IN_CANCEL, 6, 0) == 1 && out[0].kind == PROTO_IN_CANCEL);
	CHECK(FINGER(&f, PROTO_IN_UP, 6, 0) == 0); /* already over */

	/* a palm lifting before anything else happened, and a down repeated for a forwarded finger */
	fresh(&f, 160);
	FINGER(&f, PROTO_IN_DOWN, 0, 70);
	CHECK(FINGER(&f, PROTO_IN_DOWN, 0, 70) == 1); /* forwarded again: the PC treats it as a move */

	/* disabled: everything passes, even a huge contact with the pen near */
	palm_init(&f, &(struct palm_config){ .enabled = 0 });
	PEN(&f, PROTO_IN_HOVER);
	CHECK(FINGER(&f, PROTO_IN_DOWN, 0, 900) == 1);

	if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
	puts("palm: all checks passed");
	return 0;
}
