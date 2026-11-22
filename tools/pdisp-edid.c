/* pdisp-edid: write a generated EDID to a file (for inspection with edid-decode, or for tests). */
#include "edid_cli.h"

#include <stdio.h>
#include <unistd.h>

static void usage(const char *argv0)
{
	fprintf(stderr, "usage: %s [options] -o FILE\n" EDID_CLI_USAGE "  -o FILE     output file\n", argv0);
}

int edid_main(int argc, char **argv)
{
	struct edid_params p;
	const char *outfile = NULL;
	int opt;

	edid_cli_defaults(&p);
	while ((opt = getopt(argc, argv, EDID_CLI_OPTS "o:h")) != -1) {
		if (opt == 'o') {
			outfile = optarg;
			continue;
		}
		int r = edid_cli_opt(&p, opt, optarg);
		if (r != 1) {
			usage(argv[0]);
			return 2;
		}
	}
	if (!outfile) {
		usage(argv[0]);
		return 2;
	}

	uint8_t edid[EDID_SIZE];
	if (edid_generate(&p, edid) != 0) {
		fprintf(stderr, "%s: parameters cannot be encoded in an EDID base block\n", argv[0]);
		return 1;
	}
	FILE *f = fopen(outfile, "wb");
	if (!f || fwrite(edid, 1, EDID_SIZE, f) != EDID_SIZE || fclose(f) != 0) {
		perror(outfile);
		return 1;
	}
	return 0;
}
