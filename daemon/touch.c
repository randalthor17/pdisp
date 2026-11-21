#include "touch.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* ---- the two ways to deliver events: a uinput file descriptor, or a caller's callback */

struct sink {
	input_emit_fn emit;
	void *user;
	int fd; /* >= 0: uinput */
};

static int uinput_emit(void *user, uint16_t type, uint16_t code, int32_t value)
{
	struct input_event ev;
	memset(&ev, 0, sizeof ev);
	ev.type = type;
	ev.code = code;
	ev.value = value;
	return write((int)(intptr_t)user, &ev, sizeof ev) == (ssize_t)sizeof ev ? 0 : -errno;
}

static int emit(const struct sink *s, uint16_t type, uint16_t code, int32_t value) { return s->emit(s->user, type, code, value); }

static int abs_setup(int fd, uint16_t code, int32_t min, int32_t max, int32_t res)
{
	struct uinput_abs_setup a;
	memset(&a, 0, sizeof a);
	a.code = code;
	a.absinfo.minimum = min;
	a.absinfo.maximum = max;
	a.absinfo.resolution = res;
	return ioctl(fd, UI_ABS_SETUP, &a);
}

static int create_device(int fd, const char *name, unsigned short product)
{
	struct uinput_setup us;
	memset(&us, 0, sizeof us);
	us.id.bustype = BUS_VIRTUAL;
	us.id.vendor = 0x1209; /* pid.codes: open hardware and test devices */
	us.id.product = product;
	us.id.version = 1;
	snprintf(us.name, sizeof us.name, "%s", name);
	return ioctl(fd, UI_DEV_SETUP, &us) == 0 && ioctl(fd, UI_DEV_CREATE) == 0 ? 0 : -errno;
}

/* normalised 0..65535 to device units 0..size-1 */
static int32_t scale(uint16_t v, unsigned size) { return (int32_t)(((uint64_t)v * (size - 1) + 32767) / 65535); }

/* ================= touchscreen ================= */

struct touchdev {
	struct sink sink;
	unsigned slots, width, height;
	int active[TOUCH_MAX_SLOTS];
	unsigned fingers;
	int next_tracking_id;
};

static int touch_create(const struct touch_config *cfg, int fd)
{
	int res = cfg->dpi ? (int)(cfg->dpi * 10 / 254) : 0; /* units per millimetre */
	int ok = ioctl(fd, UI_SET_EVBIT, EV_KEY) == 0 && ioctl(fd, UI_SET_EVBIT, EV_ABS) == 0 && ioctl(fd, UI_SET_EVBIT, EV_SYN) == 0 &&
		 ioctl(fd, UI_SET_KEYBIT, BTN_TOUCH) == 0 && ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT) == 0 &&
		 abs_setup(fd, ABS_X, 0, (int)cfg->width - 1, res) == 0 && abs_setup(fd, ABS_Y, 0, (int)cfg->height - 1, res) == 0 &&
		 abs_setup(fd, ABS_MT_SLOT, 0, (int)cfg->slots - 1, 0) == 0 && abs_setup(fd, ABS_MT_TRACKING_ID, 0, 65535, 0) == 0 &&
		 abs_setup(fd, ABS_MT_POSITION_X, 0, (int)cfg->width - 1, res) == 0 &&
		 abs_setup(fd, ABS_MT_POSITION_Y, 0, (int)cfg->height - 1, res) == 0 && abs_setup(fd, ABS_MT_PRESSURE, 0, 255, 0) == 0;
	if (!ok) return -errno;
	return create_device(fd, cfg->name ? cfg->name : "pdisp touchscreen", 0x7064);
}

static int touch_valid(const struct touch_config *cfg) { return cfg->slots && cfg->slots <= TOUCH_MAX_SLOTS && cfg->width >= 2 && cfg->height >= 2; }

static struct touchdev *touch_alloc(const struct touch_config *cfg, const struct sink *sink)
{
	struct touchdev *t = calloc(1, sizeof *t);
	if (!t) return NULL;
	t->sink = *sink;
	t->slots = cfg->slots;
	t->width = cfg->width;
	t->height = cfg->height;
	return t;
}

int touch_open_sink(const struct touch_config *cfg, input_emit_fn fn, void *user, struct touchdev **out)
{
	if (!touch_valid(cfg)) return -EINVAL;
	struct sink s = { fn, user, -1 };
	struct touchdev *t = touch_alloc(cfg, &s);
	if (!t) return -ENOMEM;
	*out = t;
	return 0;
}

int touch_open(const struct touch_config *cfg, struct touchdev **out)
{
	if (!touch_valid(cfg)) return -EINVAL;
	int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) return -errno;
	int e = touch_create(cfg, fd);
	if (e) { close(fd); return e; }
	struct sink s = { uinput_emit, (void *)(intptr_t)fd, fd };
	struct touchdev *t = touch_alloc(cfg, &s);
	if (!t) { ioctl(fd, UI_DEV_DESTROY); close(fd); return -ENOMEM; }
	*out = t;
	return 0;
}

void touch_release_all(struct touchdev *t)
{
	if (!t->fingers) return;
	for (unsigned s = 0; s < t->slots; s++) {
		if (!t->active[s]) continue;
		emit(&t->sink, EV_ABS, ABS_MT_SLOT, (int32_t)s);
		emit(&t->sink, EV_ABS, ABS_MT_TRACKING_ID, -1);
		t->active[s] = 0;
	}
	t->fingers = 0;
	emit(&t->sink, EV_KEY, BTN_TOUCH, 0);
	emit(&t->sink, EV_SYN, SYN_REPORT, 0);
}

void touch_close(struct touchdev *t)
{
	if (!t) return;
	touch_release_all(t);
	if (t->sink.fd >= 0) {
		ioctl(t->sink.fd, UI_DEV_DESTROY);
		close(t->sink.fd);
	}
	free(t);
}

unsigned touch_fingers(const struct touchdev *t) { return t->fingers; }

int touch_event(struct touchdev *t, const struct proto_input_event *e)
{
	if (e->tool != PROTO_TOOL_FINGER) return 0; /* the stylus has its own device */
	unsigned s = e->pointer_id;
	if (s >= t->slots) return 0;
	int32_t x = scale(e->x, t->width), y = scale(e->y, t->height), p = e->pressure >> 8;
	const struct sink *k = &t->sink;
	switch (e->kind) {
	case PROTO_IN_DOWN:
		emit(k, EV_ABS, ABS_MT_SLOT, (int32_t)s);
		if (!t->active[s]) { /* (a down for a finger already down means its up was lost: it just moves) */
			emit(k, EV_ABS, ABS_MT_TRACKING_ID, t->next_tracking_id);
			t->next_tracking_id = (t->next_tracking_id + 1) & 0xffff;
			t->active[s] = 1;
			if (t->fingers++ == 0) emit(k, EV_KEY, BTN_TOUCH, 1);
		}
		break;
	case PROTO_IN_MOVE:
		if (!t->active[s]) return 0; /* a finger we never saw go down (its down was lost): ignore until it lifts and touches again */
		emit(k, EV_ABS, ABS_MT_SLOT, (int32_t)s);
		break;
	case PROTO_IN_UP:
	case PROTO_IN_CANCEL:
		if (!t->active[s]) return 0;
		emit(k, EV_ABS, ABS_MT_SLOT, (int32_t)s);
		emit(k, EV_ABS, ABS_MT_TRACKING_ID, -1);
		t->active[s] = 0;
		if (--t->fingers == 0) emit(k, EV_KEY, BTN_TOUCH, 0);
		return emit(k, EV_SYN, SYN_REPORT, 0);
	default:
		return 0; /* hover and buttons are not a touchscreen's business */
	}
	emit(k, EV_ABS, ABS_MT_POSITION_X, x);
	emit(k, EV_ABS, ABS_MT_POSITION_Y, y);
	emit(k, EV_ABS, ABS_MT_PRESSURE, p ? p : 1);
	emit(k, EV_ABS, ABS_X, x);
	emit(k, EV_ABS, ABS_Y, y);
	return emit(k, EV_SYN, SYN_REPORT, 0);
}

/* ================= pen tablet ================= */

#define PEN_PRESSURE_MAX 4095
#define PEN_DISTANCE_MAX 255
#define PEN_TILT_MAX 90

struct pendev {
	struct sink sink;
	unsigned width, height;
	int near;        /* in proximity */
	int touching;    /* the tip is down */
	int eraser;      /* the tool in proximity is the eraser end */
	uint8_t buttons; /* the button state last reported to the compositor */
};

static int pen_create(const struct pen_config *cfg, int fd)
{
	int res = cfg->dpi ? (int)(cfg->dpi * 10 / 254) : 0;
	int ok = ioctl(fd, UI_SET_EVBIT, EV_KEY) == 0 && ioctl(fd, UI_SET_EVBIT, EV_ABS) == 0 && ioctl(fd, UI_SET_EVBIT, EV_SYN) == 0 &&
		 ioctl(fd, UI_SET_KEYBIT, BTN_TOOL_PEN) == 0 && ioctl(fd, UI_SET_KEYBIT, BTN_TOOL_RUBBER) == 0 &&
		 ioctl(fd, UI_SET_KEYBIT, BTN_TOUCH) == 0 && ioctl(fd, UI_SET_KEYBIT, BTN_STYLUS) == 0 &&
		 ioctl(fd, UI_SET_KEYBIT, BTN_STYLUS2) == 0 && ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT) == 0 &&
		 abs_setup(fd, ABS_X, 0, (int)cfg->width - 1, res) == 0 && abs_setup(fd, ABS_Y, 0, (int)cfg->height - 1, res) == 0 &&
		 abs_setup(fd, ABS_PRESSURE, 0, PEN_PRESSURE_MAX, 0) == 0 && abs_setup(fd, ABS_DISTANCE, 0, PEN_DISTANCE_MAX, 0) == 0 &&
		 abs_setup(fd, ABS_TILT_X, -PEN_TILT_MAX, PEN_TILT_MAX, 0) == 0 && abs_setup(fd, ABS_TILT_Y, -PEN_TILT_MAX, PEN_TILT_MAX, 0) == 0;
	if (!ok) return -errno;
	return create_device(fd, cfg->name ? cfg->name : "pdisp stylus", 0x7065);
}

static int pen_valid(const struct pen_config *cfg) { return cfg->width >= 2 && cfg->height >= 2; }

static struct pendev *pen_alloc(const struct pen_config *cfg, const struct sink *sink)
{
	struct pendev *p = calloc(1, sizeof *p);
	if (!p) return NULL;
	p->sink = *sink;
	p->width = cfg->width;
	p->height = cfg->height;
	return p;
}

int pen_open_sink(const struct pen_config *cfg, input_emit_fn fn, void *user, struct pendev **out)
{
	if (!pen_valid(cfg)) return -EINVAL;
	struct sink s = { fn, user, -1 };
	struct pendev *p = pen_alloc(cfg, &s);
	if (!p) return -ENOMEM;
	*out = p;
	return 0;
}

int pen_open(const struct pen_config *cfg, struct pendev **out)
{
	if (!pen_valid(cfg)) return -EINVAL;
	int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) return -errno;
	int e = pen_create(cfg, fd);
	if (e) { close(fd); return e; }
	struct sink s = { uinput_emit, (void *)(intptr_t)fd, fd };
	struct pendev *p = pen_alloc(cfg, &s);
	if (!p) { ioctl(fd, UI_DEV_DESTROY); close(fd); return -ENOMEM; }
	*out = p;
	return 0;
}

int pen_in_proximity(const struct pendev *p) { return p->near; }

static void pen_buttons(struct pendev *p, uint8_t now)
{
	if ((now ^ p->buttons) & 1) emit(&p->sink, EV_KEY, BTN_STYLUS, now & 1);
	if ((now ^ p->buttons) & 2) emit(&p->sink, EV_KEY, BTN_STYLUS2, now >> 1 & 1);
	p->buttons = now;
}

static void pen_leave(struct pendev *p)
{
	if (!p->near) return;
	if (p->touching) {
		emit(&p->sink, EV_KEY, BTN_TOUCH, 0);
		emit(&p->sink, EV_ABS, ABS_PRESSURE, 0);
		p->touching = 0;
	}
	pen_buttons(p, 0);
	emit(&p->sink, EV_KEY, p->eraser ? BTN_TOOL_RUBBER : BTN_TOOL_PEN, 0);
	emit(&p->sink, EV_SYN, SYN_REPORT, 0);
	p->near = 0;
}

void pen_release_all(struct pendev *p) { pen_leave(p); }

void pen_close(struct pendev *p)
{
	if (!p) return;
	pen_leave(p);
	if (p->sink.fd >= 0) {
		ioctl(p->sink.fd, UI_DEV_DESTROY);
		close(p->sink.fd);
	}
	free(p);
}

int pen_event(struct pendev *p, const struct proto_input_event *e)
{
	if (e->tool != PROTO_TOOL_STYLUS && e->tool != PROTO_TOOL_ERASER) return 0;
	const struct sink *k = &p->sink;
	int eraser = e->tool == PROTO_TOOL_ERASER;
	if (e->kind == PROTO_IN_CANCEL) { pen_leave(p); return 0; }
	if (e->kind > PROTO_IN_BUTTON) return 0;
	if (p->near && p->eraser != eraser) pen_leave(p); /* the pen was turned over: one tool leaves before the other arrives */
	if (!p->near) { /* entering proximity */
		p->eraser = eraser;
		p->near = 1;
		emit(k, EV_KEY, eraser ? BTN_TOOL_RUBBER : BTN_TOOL_PEN, 1);
	}
	pen_buttons(p, e->buttons & 3);
	int32_t x = scale(e->x, p->width), y = scale(e->y, p->height);
	switch (e->kind) {
	case PROTO_IN_DOWN:
		if (!p->touching) { emit(k, EV_KEY, BTN_TOUCH, 1); p->touching = 1; }
		break;
	case PROTO_IN_UP:
		if (p->touching) { emit(k, EV_KEY, BTN_TOUCH, 0); p->touching = 0; }
		break;
	default:
		break;
	}
	emit(k, EV_ABS, ABS_X, x);
	emit(k, EV_ABS, ABS_Y, y);
	if (p->touching) {
		emit(k, EV_ABS, ABS_PRESSURE, e->pressure * PEN_PRESSURE_MAX / 65535);
		emit(k, EV_ABS, ABS_DISTANCE, 0);
	} else {
		emit(k, EV_ABS, ABS_PRESSURE, 0);
		emit(k, EV_ABS, ABS_DISTANCE, e->kind == PROTO_IN_HOVER ? e->pressure * PEN_DISTANCE_MAX / 65535 : 0);
	}
	emit(k, EV_ABS, ABS_TILT_X, e->tilt_x);
	emit(k, EV_ABS, ABS_TILT_Y, e->tilt_y);
	return emit(k, EV_SYN, SYN_REPORT, 0);
}
