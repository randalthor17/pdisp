#include "edid_cli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void edid_cli_defaults(struct edid_params *p)
{
	memset(p, 0, sizeof *p);
	p->width = 2400;
	p->height = 1080;
	p->refresh_mhz = 60000;
	p->mm_width = 148;
	p->mm_height = 66;
	memcpy(p->pnp_id, "PDI", 4);
	p->product_code = 1;
	p->serial = 1;
	snprintf(p->name, sizeof p->name, "pdisp");
	snprintf(p->serial_text, sizeof p->serial_text, "PDISP0001");
	p->year = 2026;
}

int edid_cli_opt(struct edid_params *p, int opt, const char *arg)
{
	double hz;
	switch (opt) {
	case 'm':
		if (sscanf(arg, "%ux%u@%lf", &p->width, &p->height, &hz) != 3) return -1;
		p->refresh_mhz = (unsigned)(hz * 1000.0 + 0.5);
		return 1;
	case 's':
		return sscanf(arg, "%ux%u", &p->mm_width, &p->mm_height) == 2 ? 1 : -1;
	case 'n':
		if (strlen(arg) > EDID_TEXT_MAX) return -1;
		snprintf(p->name, sizeof p->name, "%s", arg);
		return 1;
	case 't':
		if (strlen(arg) > EDID_TEXT_MAX) return -1;
		snprintf(p->serial_text, sizeof p->serial_text, "%s", arg);
		return 1;
	case 'N':
		p->serial = (uint32_t)strtoul(arg, NULL, 0);
		return 1;
	case 'p':
		if (strlen(arg) != 3) return -1;
		memcpy(p->pnp_id, arg, 4);
		return 1;
	case 'y':
		p->year = (unsigned)strtoul(arg, NULL, 0);
		return 1;
	default:
		return 0;
	}
}
