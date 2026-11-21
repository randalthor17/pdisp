/* pdisp-keys: look at and edit the devices this PC authorises (the daemon's authorized_keys) and its own identity.
 *
 *   pdisp-keys [-K DIR] list                    devices that may connect, with their key fingerprints
 *   pdisp-keys [-K DIR] remove FINGERPRINT      revoke: remove the device whose fingerprint starts with this (4+ characters)
 *   pdisp-keys [-K DIR] identity                this PC's own key fingerprint (what a phone pins after pairing)
 *
 * DIR defaults to ~/.config/pdisp, like pdispd. A running daemon reads the file for every new connection, so a removed device is
 * refused the next time it connects (a session that is already running is not interrupted).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "identity.h"

static void print_entry(const struct keystore_entry *e, void *user)
{
	char fp[IDENTITY_FINGERPRINT_LEN];
	identity_fingerprint(e->pub, fp);
	printf("%s  %s\n", fp, e->name[0] ? e->name : "(no name)");
	(*(int *)user)++;
}

int main(int argc, char **argv)
{
	const char *dir = NULL;
	int opt;
	while ((opt = getopt(argc, argv, "K:h")) != -1) {
		if (opt == 'K') dir = optarg;
		else { fprintf(stderr, "usage: %s [-K DIR] list | remove FINGERPRINT | identity\n", argv[0]); return 2; }
	}
	char def[900];
	if (!dir) {
		if (identity_default_dir(def, sizeof def) != 0) { fprintf(stderr, "no config directory: use -K DIR\n"); return 1; }
		dir = def;
	}
	if (optind >= argc) { fprintf(stderr, "usage: %s [-K DIR] list | remove FINGERPRINT | identity\n", argv[0]); return 2; }
	char path[1100];
	snprintf(path, sizeof path, "%s/authorized_keys", dir);
	const char *cmd = argv[optind];

	if (!strcmp(cmd, "list")) {
		int n = 0, r = keystore_list(path, print_entry, &n);
		if (r < 0) { fprintf(stderr, "cannot read %s: %s\n", path, strerror(-r)); return 1; }
		if (n == 0) fprintf(stderr, "no authorised devices\n");
		return 0;
	}
	if (!strcmp(cmd, "remove") && optind + 1 < argc) {
		int r = keystore_remove(path, argv[optind + 1]);
		if (r < 0) { fprintf(stderr, "cannot remove: %s\n", r == -22 ? "the fingerprint is too short (use at least 4 characters)" : strerror(-r)); return 1; }
		if (r == 0) { fprintf(stderr, "no device with a fingerprint starting with '%s'\n", argv[optind + 1]); return 1; }
		printf("removed %d device%s\n", r, r == 1 ? "" : "s");
		return 0;
	}
	if (!strcmp(cmd, "identity")) {
		struct identity id;
		int created, r = identity_load_or_create(dir, &id, &created);
		if (r < 0) { fprintf(stderr, "cannot use the identity in %s: %s\n", dir, strerror(-r)); return 1; }
		char fp[IDENTITY_FINGERPRINT_LEN];
		identity_fingerprint(id.pub, fp);
		printf("%s%s\n", fp, created ? "  (just created)" : "");
		return 0;
	}
	fprintf(stderr, "usage: %s [-K DIR] list | remove FINGERPRINT | identity\n", argv[0]);
	return 2;
}
