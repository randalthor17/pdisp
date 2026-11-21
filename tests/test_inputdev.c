/* The touchscreen and pen devices' event sequences, recorded through a sink instead of /dev/uinput (so no privileges are needed). */
#include <errno.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "touch.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

struct rec { struct { uint16_t type, code; int32_t value; } ev[256]; int n; };
static int record(void *user, uint16_t type, uint16_t code, int32_t value)
{
	struct rec *r = user;
	if (r->n < 256) { r->ev[r->n].type = type; r->ev[r->n].code = code; r->ev[r->n].value = value; r->n++; }
	return 0;
}
static void clear(struct rec *r) { r->n = 0; }
/* does the recording hold this event? (any position) */
static int has(const struct rec *r, uint16_t type, uint16_t code, int32_t value)
{
	for (int i = 0; i < r->n; i++) if (r->ev[i].type == type && r->ev[i].code == code && r->ev[i].value == value) return 1;
	return 0;
}
static int index_of(const struct rec *r, uint16_t type, uint16_t code, int32_t value)
{
	for (int i = 0; i < r->n; i++) if (r->ev[i].type == type && r->ev[i].code == code && r->ev[i].value == value) return i;
	return -1;
}
static int has_code(const struct rec *r, uint16_t type, uint16_t code)
{
	for (int i = 0; i < r->n; i++) if (r->ev[i].type == type && r->ev[i].code == code) return 1;
	return 0;
}
static int value_of(const struct rec *r, uint16_t type, uint16_t code)
{
	for (int i = r->n - 1; i >= 0; i--) if (r->ev[i].type == type && r->ev[i].code == code) return r->ev[i].value;
	return -99999;
}
static int syncs(const struct rec *r) { int n = 0; for (int i = 0; i < r->n; i++) n += r->ev[i].type == EV_SYN; return n; }
static int ends_with_sync(const struct rec *r) { return r->n > 0 && r->ev[r->n - 1].type == EV_SYN && r->ev[r->n - 1].code == SYN_REPORT; }

static struct proto_input_event ev(uint8_t kind, uint8_t tool, uint8_t id, uint16_t x, uint16_t y, uint16_t pressure)
{
	struct proto_input_event e;
	memset(&e, 0, sizeof e);
	e.kind = kind; e.tool = tool; e.pointer_id = id; e.x = x; e.y = y; e.pressure = pressure;
	return e;
}

/* the event builders return values, so the devices are fed through these wrappers */
static int pe(struct pendev *p, struct proto_input_event e) { return pen_event(p, &e); }
static int te(struct touchdev *t, struct proto_input_event e) { return touch_event(t, &e); }

static void pen_tests(void)
{
	struct rec r = { .n = 0 };
	struct pen_config cfg = { .name = "t", .width = 2000, .height = 1000, .dpi = 300 };
	struct pendev *p;
	CHECK(pen_open_sink(&cfg, record, &r, &p) == 0);
	pen_close(p);
	CHECK(pen_open_sink(&(struct pen_config){ .width = 1, .height = 5 }, record, &r, &p) == -EINVAL);
	CHECK(pen_open_sink(&cfg, record, &r, &p) == 0);

	/* hover: the tool enters proximity (before the position, in one frame), distance from `pressure`, no touch */
	struct proto_input_event h = ev(PROTO_IN_HOVER, PROTO_TOOL_STYLUS, 0, 32768, 16384, 32768);
	h.tilt_x = -30; h.tilt_y = 45;
	CHECK(pen_event(p, &h) == 0);
	CHECK(pen_in_proximity(p));
	CHECK(has(&r, EV_KEY, BTN_TOOL_PEN, 1) && !has_code(&r, EV_KEY, BTN_TOUCH));
	CHECK(index_of(&r, EV_KEY, BTN_TOOL_PEN, 1) < index_of(&r, EV_ABS, ABS_X, value_of(&r, EV_ABS, ABS_X)));
	CHECK(value_of(&r, EV_ABS, ABS_X) == 1000 && value_of(&r, EV_ABS, ABS_Y) == 250);   /* 0.5 of 1999, 0.25 of 999 */
	CHECK(value_of(&r, EV_ABS, ABS_DISTANCE) == 127 && value_of(&r, EV_ABS, ABS_PRESSURE) == 0);
	CHECK(value_of(&r, EV_ABS, ABS_TILT_X) == -30 && value_of(&r, EV_ABS, ABS_TILT_Y) == 45);
	CHECK(syncs(&r) == 1 && ends_with_sync(&r));

	/* hovering on: the tool is not announced again */
	clear(&r);
	pen_event(p, &(struct proto_input_event){ .kind = PROTO_IN_HOVER, .tool = PROTO_TOOL_STYLUS, .x = 40000, .y = 20000, .pressure = 0 });
	CHECK(!has_code(&r, EV_KEY, BTN_TOOL_PEN) && value_of(&r, EV_ABS, ABS_DISTANCE) == 0 && syncs(&r) == 1);

	/* the tip goes down with full pressure */
	clear(&r);
	pe(p, ev(PROTO_IN_DOWN, PROTO_TOOL_STYLUS, 0, 40000, 20000, 65535));
	CHECK(has(&r, EV_KEY, BTN_TOUCH, 1) && value_of(&r, EV_ABS, ABS_PRESSURE) == 4095 && value_of(&r, EV_ABS, ABS_DISTANCE) == 0);
	CHECK(!has_code(&r, EV_KEY, BTN_TOOL_PEN));
	/* ... moves while touching report pressure; half pressure is half */
	clear(&r);
	pe(p, ev(PROTO_IN_MOVE, PROTO_TOOL_STYLUS, 0, 41000, 21000, 32768));
	CHECK(!has_code(&r, EV_KEY, BTN_TOUCH) && value_of(&r, EV_ABS, ABS_PRESSURE) == 2047);
	/* a repeated down does not press the tip twice */
	clear(&r);
	pe(p, ev(PROTO_IN_DOWN, PROTO_TOOL_STYLUS, 0, 41000, 21000, 32768));
	CHECK(!has_code(&r, EV_KEY, BTN_TOUCH));

	/* the barrel button: pressed with an event, released with the next one */
	clear(&r);
	struct proto_input_event b = ev(PROTO_IN_MOVE, PROTO_TOOL_STYLUS, 0, 42000, 21000, 32768);
	b.buttons = 1;
	pen_event(p, &b);
	CHECK(has(&r, EV_KEY, BTN_STYLUS, 1) && !has_code(&r, EV_KEY, BTN_STYLUS2));
	clear(&r);
	pen_event(p, &b); /* unchanged: no key event */
	CHECK(!has_code(&r, EV_KEY, BTN_STYLUS));
	b.buttons = 3;
	clear(&r);
	pen_event(p, &b);
	CHECK(has(&r, EV_KEY, BTN_STYLUS2, 1) && !has_code(&r, EV_KEY, BTN_STYLUS));
	b.buttons = 0; b.kind = PROTO_IN_BUTTON;
	clear(&r);
	pen_event(p, &b);
	CHECK(has(&r, EV_KEY, BTN_STYLUS, 0) && has(&r, EV_KEY, BTN_STYLUS2, 0));

	/* the tip lifts: still near, pressure zero */
	clear(&r);
	pe(p, ev(PROTO_IN_UP, PROTO_TOOL_STYLUS, 0, 42000, 21000, 0));
	CHECK(has(&r, EV_KEY, BTN_TOUCH, 0) && value_of(&r, EV_ABS, ABS_PRESSURE) == 0 && pen_in_proximity(p));
	CHECK(!has(&r, EV_KEY, BTN_TOOL_PEN, 0));
	/* leaving proximity */
	clear(&r);
	pe(p, ev(PROTO_IN_CANCEL, PROTO_TOOL_STYLUS, 0, 0, 0, 0));
	CHECK(has(&r, EV_KEY, BTN_TOOL_PEN, 0) && !pen_in_proximity(p) && ends_with_sync(&r));
	clear(&r);
	pe(p, ev(PROTO_IN_CANCEL, PROTO_TOOL_STYLUS, 0, 0, 0, 0)); /* leaving twice: nothing */
	CHECK(r.n == 0);

	/* a cancel with the tip down and a button held lifts, releases, then leaves, in that order */
	pe(p, ev(PROTO_IN_DOWN, PROTO_TOOL_STYLUS, 0, 1000, 1000, 40000));
	struct proto_input_event held = ev(PROTO_IN_MOVE, PROTO_TOOL_STYLUS, 0, 1000, 1000, 40000);
	held.buttons = 1;
	pen_event(p, &held);
	clear(&r);
	pe(p, ev(PROTO_IN_CANCEL, PROTO_TOOL_STYLUS, 0, 0, 0, 0));
	CHECK(has(&r, EV_KEY, BTN_TOUCH, 0) && has(&r, EV_KEY, BTN_STYLUS, 0) && has(&r, EV_KEY, BTN_TOOL_PEN, 0));
	CHECK(index_of(&r, EV_KEY, BTN_TOUCH, 0) < index_of(&r, EV_KEY, BTN_TOOL_PEN, 0));
	CHECK(syncs(&r) == 1 && !pen_in_proximity(p));

	/* turning the pen over: the eraser end replaces the tip, and one leaves before the other arrives */
	pe(p, ev(PROTO_IN_HOVER, PROTO_TOOL_STYLUS, 0, 5000, 5000, 1000));
	clear(&r);
	pe(p, ev(PROTO_IN_HOVER, PROTO_TOOL_ERASER, 0, 5000, 5000, 1000));
	CHECK(has(&r, EV_KEY, BTN_TOOL_PEN, 0) && has(&r, EV_KEY, BTN_TOOL_RUBBER, 1));
	CHECK(index_of(&r, EV_KEY, BTN_TOOL_PEN, 0) < index_of(&r, EV_KEY, BTN_TOOL_RUBBER, 1));
	pe(p, ev(PROTO_IN_DOWN, PROTO_TOOL_ERASER, 0, 5000, 5000, 20000));
	clear(&r);
	pen_release_all(p); /* the session ends with the eraser pressing */
	CHECK(has(&r, EV_KEY, BTN_TOUCH, 0) && has(&r, EV_KEY, BTN_TOOL_RUBBER, 0) && !pen_in_proximity(p));
	clear(&r);
	pen_release_all(p);
	CHECK(r.n == 0);

	/* fingers and unknown kinds mean nothing to the pen */
	CHECK(pe(p, ev(PROTO_IN_DOWN, PROTO_TOOL_FINGER, 0, 1, 1, 1)) == 0 && r.n == 0 && !pen_in_proximity(p));
	struct proto_input_event odd = ev(PROTO_IN_BUTTON + 1, PROTO_TOOL_STYLUS, 0, 1, 1, 1);
	CHECK(pen_event(p, &odd) == 0 && r.n == 0);
	/* the edges of the screen */
	pe(p, ev(PROTO_IN_HOVER, PROTO_TOOL_STYLUS, 0, 0, 0, 0));
	CHECK(value_of(&r, EV_ABS, ABS_X) == 0 && value_of(&r, EV_ABS, ABS_Y) == 0);
	pe(p, ev(PROTO_IN_HOVER, PROTO_TOOL_STYLUS, 0, 65535, 65535, 0));
	CHECK(value_of(&r, EV_ABS, ABS_X) == 1999 && value_of(&r, EV_ABS, ABS_Y) == 999);
	pen_close(p);
}

static void touch_tests(void)
{
	struct rec r = { .n = 0 };
	struct touch_config cfg = { .name = "t", .width = 1080, .height = 2400, .dpi = 420, .slots = 10 };
	struct touchdev *t;
	CHECK(touch_open_sink(&cfg, record, &r, &t) == 0);
	touch_close(t);
	CHECK(touch_open_sink(&(struct touch_config){ .width = 10, .height = 10, .slots = 0 }, record, &r, &t) == -EINVAL);
	CHECK(touch_open_sink(&(struct touch_config){ .width = 10, .height = 10, .slots = 11 }, record, &r, &t) == -EINVAL);
	CHECK(touch_open_sink(&cfg, record, &r, &t) == 0);

	/* two fingers down and up; BTN_TOUCH only for the first and last */
	te(t, ev(PROTO_IN_DOWN, PROTO_TOOL_FINGER, 0, 32768, 32768, 0x8000));
	CHECK(has(&r, EV_KEY, BTN_TOUCH, 1) && value_of(&r, EV_ABS, ABS_MT_POSITION_X) == 540 && value_of(&r, EV_ABS, ABS_MT_POSITION_Y) == 1200);
	CHECK(value_of(&r, EV_ABS, ABS_MT_PRESSURE) == 128 && ends_with_sync(&r));
	int tid0 = value_of(&r, EV_ABS, ABS_MT_TRACKING_ID);
	clear(&r);
	te(t, ev(PROTO_IN_DOWN, PROTO_TOOL_FINGER, 3, 6554, 65535, 200 << 8));
	CHECK(!has_code(&r, EV_KEY, BTN_TOUCH) && has(&r, EV_ABS, ABS_MT_SLOT, 3) && value_of(&r, EV_ABS, ABS_MT_TRACKING_ID) != tid0);
	CHECK(value_of(&r, EV_ABS, ABS_MT_POSITION_X) == 108 && value_of(&r, EV_ABS, ABS_MT_POSITION_Y) == 2399 && touch_fingers(t) == 2);
	clear(&r);
	te(t, ev(PROTO_IN_UP, PROTO_TOOL_FINGER, 0, 0, 0, 0));
	CHECK(has(&r, EV_ABS, ABS_MT_TRACKING_ID, -1) && !has_code(&r, EV_KEY, BTN_TOUCH));
	clear(&r);
	te(t, ev(PROTO_IN_UP, PROTO_TOOL_FINGER, 3, 0, 0, 0));
	CHECK(has(&r, EV_KEY, BTN_TOUCH, 0) && touch_fingers(t) == 0);

	/* what must change nothing: a move for a finger that is not down, a finger beyond the slots, hover, an up with nothing down, the stylus */
	clear(&r);
	te(t, ev(PROTO_IN_MOVE, PROTO_TOOL_FINGER, 5, 1, 1, 1));
	te(t, ev(PROTO_IN_DOWN, PROTO_TOOL_FINGER, 10, 1, 1, 1));
	te(t, ev(PROTO_IN_HOVER, PROTO_TOOL_FINGER, 0, 1, 1, 1));
	te(t, ev(PROTO_IN_UP, PROTO_TOOL_FINGER, 0, 1, 1, 1));
	te(t, ev(PROTO_IN_DOWN, PROTO_TOOL_STYLUS, 0, 1, 1, 1));
	te(t, ev(PROTO_IN_DOWN, PROTO_TOOL_ERASER, 1, 1, 1, 1));
	CHECK(r.n == 0 && touch_fingers(t) == 0);

	/* a down for a finger that is already down (its up was lost) is a move, not a second finger */
	te(t, ev(PROTO_IN_DOWN, PROTO_TOOL_FINGER, 2, 100, 100, 1000));
	clear(&r);
	te(t, ev(PROTO_IN_DOWN, PROTO_TOOL_FINGER, 2, 200, 200, 1000));
	CHECK(!has_code(&r, EV_ABS, ABS_MT_TRACKING_ID) && touch_fingers(t) == 1);
	/* releasing everything */
	te(t, ev(PROTO_IN_DOWN, PROTO_TOOL_FINGER, 4, 100, 100, 1000));
	clear(&r);
	touch_release_all(t);
	CHECK(has(&r, EV_KEY, BTN_TOUCH, 0) && syncs(&r) == 1 && touch_fingers(t) == 0);
	clear(&r);
	touch_release_all(t);
	CHECK(r.n == 0);
	touch_close(t);
}

int main(void)
{
	pen_tests();
	touch_tests();
	if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
	puts("inputdev: all checks passed");
	return 0;
}
