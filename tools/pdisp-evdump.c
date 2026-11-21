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

int main(int argc, char **argv)
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
	ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &ax);
	ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &ay);
	printf("device '%s': x 0..%d, y 0..%d\n", name, ax.maximum, ay.maximum);
	fflush(stdout);
	int slot = 0, x[16] = { 0 }, y[16] = { 0 }, tid[16], touched[16] = { 0 };
	for (int i = 0; i < 16; i++) tid[i] = -1;
	for (;;) {
		clock_gettime(CLOCK_MONOTONIC, &t);
		if ((t.tv_sec - t0.tv_sec) >= secs) break;
		struct pollfd pf = { .fd = fd, .events = POLLIN };
		if (poll(&pf, 1, 100) <= 0) continue;
		struct input_event e;
		while (read(fd, &e, sizeof e) == (ssize_t)sizeof e) {
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
