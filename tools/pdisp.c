/* pdisp: the command line of pdisp. One program with subcommands:
 *   pdisp status | watch | devices | remove | set | disconnect | pairing | answer   the running daemon (docs/control.md)
 *   pdisp keys list | remove | identity                                              the authorised devices (offline)
 *   pdisp brightness [N | +N | -N]                                                   the phone's brightness over DDC/CI
 *   pdisp gui [--background]                                                         the tray application
 */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "pdisp-cli.h"

static void usage(FILE *f)
{
	fprintf(f,
		"usage: pdisp [-s SOCKET] status | watch [N] | devices | remove FINGERPRINT | set KEY VALUE | disconnect | pairing [SECS] | answer ID always|once|deny\n"
		"       pdisp keys [-K DIR] list | remove FINGERPRINT | identity\n"
		"       pdisp brightness [-d /dev/i2c-N] [N | +N | -N]\n"
		"       pdisp gui [--background]\n");
}

/* The tray application is a separate program (Qt): next to this one in a build tree, in libexec when installed, or on the PATH. */
static int run_gui(int argc, char **argv)
{
	char self[PATH_MAX], cand[PATH_MAX + 64];
	ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
	char *gui_argv[argc + 1];
	gui_argv[0] = "pdisp-gui";
	for (int i = 2; i < argc; i++) gui_argv[i - 1] = argv[i];
	gui_argv[argc > 1 ? argc - 1 : 1] = NULL;
	if (n > 0) {
		self[n] = 0;
		char *slash = strrchr(self, '/');
		if (slash) {
			*slash = 0;
			const char *rel[] = { "../libexec/pdisp/pdisp-gui", "gui/pdisp-gui" };
			for (int i = 0; i < 2; i++) {
				snprintf(cand, sizeof cand, "%s/%s", self, rel[i]);
				if (access(cand, X_OK) == 0) { execv(cand, gui_argv); perror(cand); return 1; }
			}
		}
	}
	execvp("pdisp-gui", gui_argv);
	fprintf(stderr, "pdisp: the tray application is not installed (pdisp was built without it)\n");
	return 1;
}

int main(int argc, char **argv)
{
	if (argc >= 2 && (!strcmp(argv[1], "help") || !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) { usage(stdout); return 0; }
	if (argc < 2) { usage(stderr); return 2; }
	if (!strcmp(argv[1], "gui")) return run_gui(argc, argv);
	if (!strcmp(argv[1], "keys")) { argv[1] = "pdisp keys"; return keys_main(argc - 1, argv + 1); }
	if (!strcmp(argv[1], "brightness")) return brightness_main(argc - 1, argv + 1);
	return ctl_main(argc, argv);
}
