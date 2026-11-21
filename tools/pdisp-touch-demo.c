/* pdisp-touch-demo: creates the virtual touchscreen the daemon would create and injects a scripted set of touches, to see where the
 * compositor puts them. Used by tests and for experiments; the daemon does this itself for a real phone.
 *
 *   pdisp-touch-demo W H [-d DPI] [-w SECS] [-s SCRIPT]
 *
 * SCRIPT is a list of steps separated by spaces: tap:X,Y (a finger down and up at the fraction X,Y of the screen, 0..1),
 * swipe:X1,Y1:X2,Y2, pinch:CX,CY:R1:R2 (two fingers around CX,CY moving from distance R1 to R2), hold:MS. The default is one tap in the
 * middle. Prints "injected ..." for each step. Waits SECS (default 1) before the first one so a client can set itself up.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "touch.h"

static void nap(int ms) { nanosleep(&(struct timespec){ ms / 1000, (ms % 1000) * 1000000L }, NULL); }
static uint16_t n16(double f) { return (uint16_t)(f < 0 ? 0 : f > 1 ? 65535 : f * 65535 + 0.5); }

static void send(struct touchdev *t, uint8_t kind, uint8_t id, double x, double y)
{
	struct proto_input_event e;
	memset(&e, 0, sizeof e);
	e.kind = kind; e.pointer_id = id; e.x = n16(x); e.y = n16(y); e.pressure = 30000;
	touch_event(t, &e);
}

int main(int argc, char **argv)
{
	if (argc < 3) { fprintf(stderr, "usage: %s W H [-d DPI] [-w SECS] [-s SCRIPT]\n", argv[0]); return 2; }
	unsigned w = (unsigned)atoi(argv[1]), h = (unsigned)atoi(argv[2]), dpi = 160;
	double wait = 1;
	const char *script = "tap:0.5,0.5";
	for (int i = 3; i + 1 < argc; i += 2) {
		if (!strcmp(argv[i], "-d")) dpi = (unsigned)atoi(argv[i + 1]);
		else if (!strcmp(argv[i], "-w")) wait = atof(argv[i + 1]);
		else if (!strcmp(argv[i], "-s")) script = argv[i + 1];
	}
	struct touch_config cfg = { .name = "pdisp touchscreen", .width = w, .height = h, .dpi = dpi, .slots = 10 };
	struct touchdev *t;
	int r = touch_open(&cfg, &t);
	if (r) { fprintf(stderr, "cannot create the touch device: %s\n", strerror(-r)); return 1; }
	printf("created a %ux%u touchscreen\n", w, h);
	fflush(stdout);
	nap((int)(wait * 1000));

	char *copy = strdup(script), *save = NULL;
	for (char *step = strtok_r(copy, " ", &save); step; step = strtok_r(NULL, " ", &save)) {
		double a, b, c, d;
		int ms;
		if (sscanf(step, "tap:%lf,%lf", &a, &b) == 2) {
			send(t, PROTO_IN_DOWN, 0, a, b); nap(60); send(t, PROTO_IN_UP, 0, a, b);
		} else if (sscanf(step, "swipe:%lf,%lf:%lf,%lf", &a, &b, &c, &d) == 4) {
			send(t, PROTO_IN_DOWN, 0, a, b);
			for (int i = 1; i <= 20; i++) { nap(8); send(t, PROTO_IN_MOVE, 0, a + (c - a) * i / 20, b + (d - b) * i / 20); }
			send(t, PROTO_IN_UP, 0, c, d);
		} else if (sscanf(step, "pinch:%lf,%lf:%lf:%lf", &a, &b, &c, &d) == 4) {
			send(t, PROTO_IN_DOWN, 0, a - c, b); send(t, PROTO_IN_DOWN, 1, a + c, b);
			for (int i = 1; i <= 20; i++) {
				double rr = c + (d - c) * i / 20;
				nap(8); send(t, PROTO_IN_MOVE, 0, a - rr, b); send(t, PROTO_IN_MOVE, 1, a + rr, b);
			}
			send(t, PROTO_IN_UP, 0, a - d, b); send(t, PROTO_IN_UP, 1, a + d, b);
		} else if (sscanf(step, "hold:%d", &ms) == 1) {
			nap(ms);
			continue;
		} else { fprintf(stderr, "unknown step '%s'\n", step); continue; }
		printf("injected %s\n", step);
		fflush(stdout);
		nap(150);
	}
	free(copy);
	nap(500);
	touch_close(t);
	return 0;
}
