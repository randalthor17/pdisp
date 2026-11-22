/* pdisp-dev: the development and diagnostic tools in one program, `pdisp-dev <tool> ...` (or a link named pdisp-<tool>, as the tests use).
 *   edid, vdisplay, capture, encode, evdump, touch-demo
 * pdisp-view (a stand-in phone, SDL and ffmpeg) and pdisp-pattern (a Wayland test window) stay separate programs: they need libraries these do not. */
#include <stdio.h>
#include <string.h>

#include "pdisp-dev.h"

static const struct { const char *name; int (*fn)(int, char **); } TOOLS[] = {
	{ "edid", edid_main }, { "vdisplay", vdisplay_main }, { "capture", capture_main },
	{ "encode", encode_main }, { "evdump", evdump_main }, { "touch-demo", touch_demo_main },
};

int main(int argc, char **argv)
{
	const char *base = strrchr(argv[0], '/') ? strrchr(argv[0], '/') + 1 : argv[0];
	int shift = 0;
	const char *tool = NULL;
	if (!strncmp(base, "pdisp-", 6) && strcmp(base, "pdisp-dev")) tool = base + 6; /* started through a link named pdisp-<tool> */
	else if (argc >= 2) { tool = argv[1]; shift = 1; }
	for (size_t i = 0; tool && i < sizeof TOOLS / sizeof *TOOLS; i++) {
		if (strcmp(tool, TOOLS[i].name)) continue;
		argv[shift] = argv[0]; /* the tool's own name for messages */
		return TOOLS[i].fn(argc - shift, argv + shift);
	}
	fprintf(stderr, "usage: pdisp-dev <tool> [arguments]\ntools:");
	for (size_t i = 0; i < sizeof TOOLS / sizeof *TOOLS; i++) fprintf(stderr, " %s", TOOLS[i].name);
	fputc('\n', stderr);
	return 2;
}
