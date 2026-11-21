/* The uinput touchscreen: what the compositor would read from its evdev node, event by event. Needs /dev/uinput; skipped (77) if it
 * cannot be opened or the new device's node cannot be read. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "touch.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

/* finds /dev/input/eventN of the device with this name */
static int open_node(const char *name)
{
	for (int attempt = 0; attempt < 50; attempt++) {
		for (int i = 0; i < 64; i++) {
			char p[128], nm[128] = "";
			snprintf(p, sizeof p, "/sys/class/input/event%d/device/name", i);
			FILE *f = fopen(p, "r");
			if (!f) continue;
			if (fgets(nm, sizeof nm, f)) nm[strcspn(nm, "\n")] = 0;
			fclose(f);
			if (strcmp(nm, name) == 0) {
				snprintf(p, sizeof p, "/dev/input/event%d", i);
				int fd = open(p, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
				if (fd >= 0) return fd;
				if (errno != ENOENT) return -errno - 1000; /* it exists but we may not read it */
			}
		}
		nanosleep(&(struct timespec){ 0, 100000000 }, NULL); /* udev may still be setting permissions */
	}
	return -1;
}

/* the events of one SYN_REPORT frame, as "type:code=value" strings */
struct frame { struct input_event ev[32]; int n; };
static int read_frame(int fd, struct frame *f)
{
	f->n = 0;
	for (int tries = 0; tries < 200; tries++) {
		struct input_event e;
		ssize_t r = read(fd, &e, sizeof e);
		if (r != (ssize_t)sizeof e) { nanosleep(&(struct timespec){ 0, 10000000 }, NULL); continue; }
		if (e.type == EV_SYN && e.code == SYN_REPORT) return 1;
		if (f->n < 32) f->ev[f->n++] = e;
	}
	return 0;
}
static int has(const struct frame *f, int type, int code, int value)
{
	for (int i = 0; i < f->n; i++) if (f->ev[i].type == type && f->ev[i].code == code && f->ev[i].value == value) return 1;
	return 0;
}
static int value_of(const struct frame *f, int type, int code, int *v)
{
	for (int i = 0; i < f->n; i++) if (f->ev[i].type == type && f->ev[i].code == code) { *v = f->ev[i].value; return 1; }
	return 0;
}

int main(void)
{
	char name[64];
	snprintf(name, sizeof name, "pdisp test touchscreen %d", (int)getpid());
	struct touch_config cfg = { .name = name, .width = 1080, .height = 2400, .dpi = 420, .slots = 10 };
	struct touchdev *t;
	int r = touch_open(&cfg, &t);
	if (r == -ENOENT || r == -EACCES) { puts("SKIP: cannot open /dev/uinput"); return 77; }
	CHECK(r == 0);
	if (r) return 1;
	int fd = open_node(name);
	if (fd < 0) { puts("SKIP: cannot read the new device's event node"); touch_close(t); return 77; }

	/* the device describes itself as a direct multitouch screen with the right ranges */
	unsigned long props[2] = { 0 };
	CHECK(ioctl(fd, EVIOCGPROP(sizeof props), props) >= 0 && (props[0] >> INPUT_PROP_DIRECT & 1));
	struct input_absinfo ai;
	CHECK(ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &ai) == 0 && ai.maximum == 1079 && ai.resolution == 16);
	CHECK(ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &ai) == 0 && ai.maximum == 2399);
	CHECK(ioctl(fd, EVIOCGABS(ABS_MT_SLOT), &ai) == 0 && ai.maximum == 9);

	struct frame f;
	int v;
	/* finger 0 down at the centre */
	CHECK(touch_event(t, &(struct proto_input_event){ .kind = PROTO_IN_DOWN, .pointer_id = 0, .x = 32768, .y = 32768, .pressure = 0x8000 }) == 0);
	CHECK(read_frame(fd, &f));
	/* (the kernel does not repeat an event that changes nothing, so slot 0 is not announced: it is already the current slot) */
	CHECK(has(&f, EV_KEY, BTN_TOUCH, 1));
	CHECK(value_of(&f, EV_ABS, ABS_MT_TRACKING_ID, &v) && v >= 0);
	int tid0 = v;
	CHECK(value_of(&f, EV_ABS, ABS_MT_POSITION_X, &v) && v == 540);
	CHECK(value_of(&f, EV_ABS, ABS_MT_POSITION_Y, &v) && v == 1200);
	CHECK(value_of(&f, EV_ABS, ABS_MT_PRESSURE, &v) && v == 128);
	CHECK(touch_fingers(t) == 1);

	/* finger 3 down: a second slot with its own tracking id, and BTN_TOUCH is not pressed again */
	touch_event(t, &(struct proto_input_event){ .kind = PROTO_IN_DOWN, .pointer_id = 3, .x = 6554, .y = 65535, .pressure = 200 });
	CHECK(read_frame(fd, &f));
	CHECK(has(&f, EV_ABS, ABS_MT_SLOT, 3) && !value_of(&f, EV_KEY, BTN_TOUCH, &v));
	CHECK(value_of(&f, EV_ABS, ABS_MT_TRACKING_ID, &v) && v != tid0);
	CHECK(value_of(&f, EV_ABS, ABS_MT_POSITION_X, &v) && v == 108);   /* 6554 / 65535 of 1079 */
	CHECK(value_of(&f, EV_ABS, ABS_MT_POSITION_Y, &v) && v == 2399);

	/* a move of finger 0 */
	touch_event(t, &(struct proto_input_event){ .kind = PROTO_IN_MOVE, .pointer_id = 0, .x = 65535, .y = 0, .pressure = 100 });
	CHECK(read_frame(fd, &f));
	CHECK(has(&f, EV_ABS, ABS_MT_SLOT, 0) && value_of(&f, EV_ABS, ABS_MT_POSITION_X, &v) && v == 1079);
	CHECK(touch_fingers(t) == 2);

	/* finger 0 up: its tracking id goes to -1, BTN_TOUCH stays down because finger 3 is */
	touch_event(t, &(struct proto_input_event){ .kind = PROTO_IN_UP, .pointer_id = 0 });
	CHECK(read_frame(fd, &f));
	CHECK(has(&f, EV_ABS, ABS_MT_TRACKING_ID, -1) && !value_of(&f, EV_KEY, BTN_TOUCH, &v));

	/* things that must change nothing: a move for a finger that is not down, a finger beyond the slots, hover, a repeated up */
	touch_event(t, &(struct proto_input_event){ .kind = PROTO_IN_MOVE, .pointer_id = 5, .x = 1, .y = 1 });
	touch_event(t, &(struct proto_input_event){ .kind = PROTO_IN_DOWN, .pointer_id = 10, .x = 1, .y = 1 });
	touch_event(t, &(struct proto_input_event){ .kind = PROTO_IN_HOVER, .pointer_id = 0, .x = 1, .y = 1 });
	touch_event(t, &(struct proto_input_event){ .kind = PROTO_IN_UP, .pointer_id = 0 });
	CHECK(touch_fingers(t) == 1);
	struct input_event e;
	CHECK(read(fd, &e, sizeof e) != (ssize_t)sizeof e); /* nothing was emitted for any of them */

	/* releasing everything lifts the remaining finger and BTN_TOUCH */
	touch_release_all(t);
	CHECK(read_frame(fd, &f));
	CHECK(has(&f, EV_ABS, ABS_MT_SLOT, 3) && has(&f, EV_ABS, ABS_MT_TRACKING_ID, -1) && has(&f, EV_KEY, BTN_TOUCH, 0));
	CHECK(touch_fingers(t) == 0);

	/* tracking ids keep counting up across touches */
	touch_event(t, &(struct proto_input_event){ .kind = PROTO_IN_DOWN, .pointer_id = 0, .x = 100, .y = 100, .pressure = 100 });
	CHECK(read_frame(fd, &f) && value_of(&f, EV_ABS, ABS_MT_TRACKING_ID, &v) && v != tid0);
	touch_event(t, &(struct proto_input_event){ .kind = PROTO_IN_CANCEL, .pointer_id = 0 });
	CHECK(read_frame(fd, &f) && has(&f, EV_ABS, ABS_MT_TRACKING_ID, -1) && has(&f, EV_KEY, BTN_TOUCH, 0));

	/* closing the device with a finger down lifts it first */
	touch_event(t, &(struct proto_input_event){ .kind = PROTO_IN_DOWN, .pointer_id = 2, .x = 5, .y = 5, .pressure = 10 });
	CHECK(read_frame(fd, &f));
	touch_close(t); /* lifts the finger, then removes the device (its node is gone afterwards, so there is nothing to read) */
	close(fd);

	CHECK(touch_open(&(struct touch_config){ .name = "x", .width = 10, .height = 10, .slots = 0 }, &t) == -EINVAL);
	CHECK(touch_open(&(struct touch_config){ .name = "x", .width = 10, .height = 10, .slots = 11 }, &t) == -EINVAL);
	if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
	puts("touch: all checks passed");
	return 0;
}
