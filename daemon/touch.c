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

struct touchdev {
	int fd;
	unsigned slots, width, height;
	int active[TOUCH_MAX_SLOTS];
	unsigned fingers;
	int next_tracking_id;
};

static int emit(int fd, uint16_t type, uint16_t code, int32_t value)
{
	struct input_event ev;
	memset(&ev, 0, sizeof ev);
	ev.type = type;
	ev.code = code;
	ev.value = value;
	return write(fd, &ev, sizeof ev) == (ssize_t)sizeof ev ? 0 : -errno;
}

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

int touch_open(const struct touch_config *cfg, struct touchdev **out)
{
	if (!cfg->slots || cfg->slots > TOUCH_MAX_SLOTS || cfg->width < 2 || cfg->height < 2) return -EINVAL;
	int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) return -errno;
	int res = cfg->dpi ? (int)(cfg->dpi * 10 / 254) : 0; /* units per millimetre */
	int ok = ioctl(fd, UI_SET_EVBIT, EV_KEY) == 0 && ioctl(fd, UI_SET_EVBIT, EV_ABS) == 0 && ioctl(fd, UI_SET_EVBIT, EV_SYN) == 0 &&
		 ioctl(fd, UI_SET_KEYBIT, BTN_TOUCH) == 0 && ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT) == 0 &&
		 abs_setup(fd, ABS_X, 0, (int)cfg->width - 1, res) == 0 && abs_setup(fd, ABS_Y, 0, (int)cfg->height - 1, res) == 0 &&
		 abs_setup(fd, ABS_MT_SLOT, 0, (int)cfg->slots - 1, 0) == 0 && abs_setup(fd, ABS_MT_TRACKING_ID, 0, 65535, 0) == 0 &&
		 abs_setup(fd, ABS_MT_POSITION_X, 0, (int)cfg->width - 1, res) == 0 &&
		 abs_setup(fd, ABS_MT_POSITION_Y, 0, (int)cfg->height - 1, res) == 0 && abs_setup(fd, ABS_MT_PRESSURE, 0, 255, 0) == 0;
	if (!ok) { int e = errno; close(fd); return -e; }
	struct uinput_setup us;
	memset(&us, 0, sizeof us);
	us.id.bustype = BUS_VIRTUAL;
	us.id.vendor = 0x1209; /* pid.codes: open hardware and test devices */
	us.id.product = 0x7064;
	us.id.version = 1;
	snprintf(us.name, sizeof us.name, "%s", cfg->name ? cfg->name : "pdisp touchscreen");
	if (ioctl(fd, UI_DEV_SETUP, &us) != 0 || ioctl(fd, UI_DEV_CREATE) != 0) { int e = errno; close(fd); return -e; }
	struct touchdev *t = calloc(1, sizeof *t);
	if (!t) { ioctl(fd, UI_DEV_DESTROY); close(fd); return -ENOMEM; }
	t->fd = fd;
	t->slots = cfg->slots;
	t->width = cfg->width;
	t->height = cfg->height;
	*out = t;
	return 0;
}

void touch_release_all(struct touchdev *t)
{
	if (!t->fingers) return;
	for (unsigned s = 0; s < t->slots; s++) {
		if (!t->active[s]) continue;
		emit(t->fd, EV_ABS, ABS_MT_SLOT, (int32_t)s);
		emit(t->fd, EV_ABS, ABS_MT_TRACKING_ID, -1);
		t->active[s] = 0;
	}
	t->fingers = 0;
	emit(t->fd, EV_KEY, BTN_TOUCH, 0);
	emit(t->fd, EV_SYN, SYN_REPORT, 0);
}

void touch_close(struct touchdev *t)
{
	if (!t) return;
	touch_release_all(t);
	ioctl(t->fd, UI_DEV_DESTROY);
	close(t->fd);
	free(t);
}

unsigned touch_fingers(const struct touchdev *t) { return t->fingers; }

/* normalised 0..65535 to device units */
static int32_t scale(uint16_t v, unsigned size) { return (int32_t)(((uint64_t)v * (size - 1) + 32767) / 65535); }

int touch_event(struct touchdev *t, const struct proto_input_event *e)
{
	unsigned s = e->pointer_id;
	if (s >= t->slots) return 0;
	int32_t x = scale(e->x, t->width), y = scale(e->y, t->height), p = e->pressure >> 8;
	switch (e->kind) {
	case PROTO_IN_DOWN:
		if (t->active[s]) { /* a down for a finger already down: the earlier up was lost; treat it as a move */
			emit(t->fd, EV_ABS, ABS_MT_SLOT, (int32_t)s);
		} else {
			emit(t->fd, EV_ABS, ABS_MT_SLOT, (int32_t)s);
			emit(t->fd, EV_ABS, ABS_MT_TRACKING_ID, t->next_tracking_id);
			t->next_tracking_id = (t->next_tracking_id + 1) & 0xffff;
			t->active[s] = 1;
			if (t->fingers++ == 0) emit(t->fd, EV_KEY, BTN_TOUCH, 1);
		}
		break;
	case PROTO_IN_MOVE:
		if (!t->active[s]) return 0; /* a finger we never saw go down (its down was lost): ignore until it lifts and touches again */
		emit(t->fd, EV_ABS, ABS_MT_SLOT, (int32_t)s);
		break;
	case PROTO_IN_UP:
	case PROTO_IN_CANCEL:
		if (!t->active[s]) return 0;
		emit(t->fd, EV_ABS, ABS_MT_SLOT, (int32_t)s);
		emit(t->fd, EV_ABS, ABS_MT_TRACKING_ID, -1);
		t->active[s] = 0;
		if (--t->fingers == 0) emit(t->fd, EV_KEY, BTN_TOUCH, 0);
		return emit(t->fd, EV_SYN, SYN_REPORT, 0);
	default:
		return 0; /* hover and stylus buttons: not a touchscreen's business */
	}
	emit(t->fd, EV_ABS, ABS_MT_POSITION_X, x);
	emit(t->fd, EV_ABS, ABS_MT_POSITION_Y, y);
	emit(t->fd, EV_ABS, ABS_MT_PRESSURE, p ? p : 1);
	emit(t->fd, EV_ABS, ABS_X, x);
	emit(t->fd, EV_ABS, ABS_Y, y);
	return emit(t->fd, EV_SYN, SYN_REPORT, 0);
}
