/* pdisp-evdump: prints the multitouch events of the "pdisp touchscreen" device, one line per frame, for tests that must not depend on
 * the compositor. The node is root-only, so run it with pkexec/sudo.   pdisp-evdump [-n DEVICE_NAME] [-t SECS] */
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int find_node(const char *name)
{
	for (int i = 0; i < 64; i++) {
		char p[128], nm[128] = "";
		snprintf(p, sizeof p, "/sys/class/input/event%d/device/name", i);
		FILE *f = fopen(p, "r");
		if (!f) continue;
		if (fgets(nm, sizeof nm, f)) nm[strcspn(nm, "\n")] = 0;
		fclose(f);
		if (strcmp(nm, name) == 0) {
			snprintf(p, sizeof p, "/dev/input/event%d", i);
			return open(p, O_RDONLY | O_CLOEXEC);
		}
	}
	return -1;
}

int evdump_main(int argc, char **argv)
{
	const char *name = "pdisp touchscreen";
	double secs = 20;
	int opt;
	while ((opt = getopt(argc, argv, "n:t:")) != -1) {
		if (opt == 'n') name = optarg;
		else if (opt == 't') secs = atof(optarg);
	}
	struct timespec t0, t;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	int fd = -1;
	while (fd < 0) { /* the device appears when a phone connects */
		fd = find_node(name);
		clock_gettime(CLOCK_MONOTONIC, &t);
		if (fd < 0 && (t.tv_sec - t0.tv_sec) >= secs) { fprintf(stderr, "no device named '%s'\n", name); return 1; }
		if (fd < 0) nanosleep(&(struct timespec){ 0, 100000000 }, NULL);
	}
	struct input_absinfo ax, ay;
	unsigned long absbits[(ABS_MAX + 8 * sizeof(long)) / (8 * sizeof(long))] = { 0 };
	ioctl(fd, EVIOCGBIT(EV_ABS, sizeof absbits), absbits);
	int pen = !(absbits[ABS_MT_POSITION_X / (8 * sizeof(long))] >> (ABS_MT_POSITION_X % (8 * sizeof(long))) & 1); /* no multitouch axes: the pen tablet */
	if (pen) { ioctl(fd, EVIOCGABS(ABS_X), &ax); ioctl(fd, EVIOCGABS(ABS_Y), &ay); } else ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &ay);
	printf("device '%s' (%s): x 0..%d, y 0..%d\n", name, pen ? "pen" : "touch", ax.maximum, ay.maximum);
	fflush(stdout);
	int px = 0, py = 0, pressure = 0, distance = 0, tiltx = 0, tilty = 0, ptouch = 0, ptool = 0, pbtn = 0, pdirty = 0;
	int slot = 0, x[16] = { 0 }, y[16] = { 0 }, tid[16], touched[16] = { 0 };
	for (int i = 0; i < 16; i++) tid[i] = -1;
	for (;;) {
		clock_gettime(CLOCK_MONOTONIC, &t);
		if ((t.tv_sec - t0.tv_sec) >= secs) break;
		struct pollfd pf = { .fd = fd, .events = POLLIN };
		if (poll(&pf, 1, 100) <= 0) continue;
		struct input_event e;
		while (read(fd, &e, sizeof e) == (ssize_t)sizeof e) {
			if (pen) {
				if (e.type == EV_ABS && e.code == ABS_X) { px = e.value; pdirty = 1; }
				else if (e.type == EV_ABS && e.code == ABS_Y) { py = e.value; pdirty = 1; }
				else if (e.type == EV_ABS && e.code == ABS_PRESSURE) pressure = e.value;
				else if (e.type == EV_ABS && e.code == ABS_DISTANCE) distance = e.value;
				else if (e.type == EV_ABS && e.code == ABS_TILT_X) tiltx = e.value;
				else if (e.type == EV_ABS && e.code == ABS_TILT_Y) tilty = e.value;
				else if (e.type == EV_KEY && e.code == BTN_TOUCH) { ptouch = e.value; pdirty = 1; }
				else if (e.type == EV_KEY && e.code == BTN_TOOL_PEN) { ptool = e.value ? 1 : 0; pdirty = 1; printf("%s\n", e.value ? "pen in" : "pen out"); }
				else if (e.type == EV_KEY && e.code == BTN_TOOL_RUBBER) { ptool = e.value ? 2 : 0; pdirty = 1; printf("%s\n", e.value ? "eraser in" : "eraser out"); }
				else if (e.type == EV_KEY && (e.code == BTN_STYLUS || e.code == BTN_STYLUS2)) { pbtn = e.value; printf("button %d %s\n", e.code == BTN_STYLUS ? 1 : 2, e.value ? "down" : "up"); }
				else if (e.type == EV_SYN && e.code == SYN_REPORT && pdirty) {
					if (ptool) printf("%s x=%d y=%d pressure=%d distance=%d tilt=%d,%d%s\n", ptouch ? "draw" : "hover", px, py, pressure, distance, tiltx, tilty, pbtn ? " button" : "");
					pdirty = 0;
					fflush(stdout);
				}
				continue;
			}
			if (e.type == EV_ABS && e.code == ABS_MT_SLOT && e.value < 16) slot = e.value;
			else if (e.type == EV_ABS && e.code == ABS_MT_TRACKING_ID) { tid[slot] = e.value; touched[slot] = 1; }
			else if (e.type == EV_ABS && e.code == ABS_MT_POSITION_X) { x[slot] = e.value; touched[slot] = 1; }
			else if (e.type == EV_ABS && e.code == ABS_MT_POSITION_Y) { y[slot] = e.value; touched[slot] = 1; }
			else if (e.type == EV_SYN && e.code == SYN_REPORT) {
				for (int s = 0; s < 16; s++) {
					if (!touched[s]) continue;
					if (tid[s] < 0) printf("up slot=%d\n", s);
					else printf("%s slot=%d x=%d y=%d\n", "touch", s, x[s], y[s]);
					touched[s] = 0;
				}
				fflush(stdout);
			}
		}
	}
	return 0;
}
