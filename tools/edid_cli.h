/* Shared command-line parsing for tools that take EDID parameters. */
#ifndef PDISP_EDID_CLI_H
#define PDISP_EDID_CLI_H

#include "edid.h"

/* getopt letters handled by edid_cli_opt(): append to a tool's own optstring */
#define EDID_CLI_OPTS "m:s:n:t:N:p:y:"
#define EDID_CLI_USAGE \
	"  -m WxH@HZ   mode (default 2400x1080@60)\n" \
	"  -s MMWxMMH  physical size in mm (default 148x66)\n" \
	"  -n NAME     monitor name, up to 13 characters (default pdisp)\n" \
	"  -t TEXT     serial text, up to 13 characters (default PDISP0001)\n" \
	"  -N SERIAL   binary serial (default 1)\n" \
	"  -p PNP      3-letter manufacturer id (default PDI)\n" \
	"  -y YEAR     year of manufacture (default 2026)\n"

void edid_cli_defaults(struct edid_params *p);

/* Returns 1 if `opt` was an EDID option and was applied, 0 if it is not one, -1 if its argument is malformed. */
int edid_cli_opt(struct edid_params *p, int opt, const char *arg);

#endif
