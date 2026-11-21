/* pdisp-edid: write a generated EDID to a file (for inspection with edid-decode, or for tests). */
#include "edid.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s -m WxH@HZ -s MMWxMMH [-n name] [-t serial-text] [-N serial] [-p PNP] [-y year] -o FILE\n"
		"  example: %s -m 2304x1440@90 -s 234x150 -n pdisp-tablet -t PDISP0001 -o tablet.edid\n",
		argv0, argv0);
}

int main(int argc, char **argv)
{
	struct edid_params p = { .width = 2400, .height = 1080, .refresh_mhz = 60000, .mm_width = 148, .mm_height = 66,
				 .pnp_id = "PDI", .product_code = 1, .serial = 1, .name = "pdisp", .serial_text = "PDISP0001",
				 .year = 2026 };
	const char *outfile = NULL;
	int opt;

	while ((opt = getopt(argc, argv, "m:s:n:t:N:p:y:o:h")) != -1) {
		double hz;
		switch (opt) {
		case 'm':
			if (sscanf(optarg, "%ux%u@%lf", &p.width, &p.height, &hz) != 3) { usage(argv[0]); return 2; }
			p.refresh_mhz = (unsigned)(hz * 1000.0 + 0.5);
			break;
		case 's':
			if (sscanf(optarg, "%ux%u", &p.mm_width, &p.mm_height) != 2) { usage(argv[0]); return 2; }
			break;
		case 'n': snprintf(p.name, sizeof p.name, "%s", optarg); break;
		case 't': snprintf(p.serial_text, sizeof p.serial_text, "%s", optarg); break;
		case 'N': p.serial = (uint32_t)strtoul(optarg, NULL, 0); break;
		case 'p': snprintf(p.pnp_id, sizeof p.pnp_id, "%s", optarg); break;
		case 'y': p.year = (unsigned)strtoul(optarg, NULL, 0); break;
		case 'o': outfile = optarg; break;
		default: usage(argv[0]); return 2;
		}
	}
	if (!outfile) { usage(argv[0]); return 2; }

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
