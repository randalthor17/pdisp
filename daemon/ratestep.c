#include "ratestep.h"

#include <string.h>

void ratestep_reset(struct ratestep *r)
{
	memset(r, 0, sizeof *r);
	for (int i = 0; i < RATESTEP_WINDOW; i++) r->bad[i] = 2;
}

enum ratestep_action ratestep_tick(struct ratestep *r, const struct ratestep_sample *s)
{
	if (!s->active) return RATESTEP_NONE; /* idle: neither evidence for nor against */
	bool bad = s->idr_unresolved > 0 || s->frames_lost > 0 || s->phone_slow;
	memmove(r->bad, r->bad + 1, RATESTEP_WINDOW - 1);
	r->bad[RATESTEP_WINDOW - 1] = bad ? 1 : 0;
	if (r->n < 1000) r->n++;
	r->good_run = bad ? 0 : r->good_run + 1;
	unsigned seen = 0, nbad = 0;
	for (int i = 0; i < RATESTEP_WINDOW; i++) { if (r->bad[i] != 2) seen++; if (r->bad[i] == 1) nbad++; }
	if (seen == RATESTEP_WINDOW && nbad >= RATESTEP_BAD_NEEDED) { ratestep_reset(r); return RATESTEP_DOWN; }
	if (!r->stable_reported && r->good_run >= RATESTEP_STABLE_TICKS) { r->stable_reported = true; return RATESTEP_STABLE; }
	return RATESTEP_NONE;
}

unsigned ratestep_next_lower(const uint32_t *offered_mhz, int n, unsigned current_mhz, unsigned floor_mhz)
{
	unsigned best = 0;
	for (int i = 0; i < n; i++)
		if (offered_mhz[i] < current_mhz && offered_mhz[i] >= floor_mhz && offered_mhz[i] > best) best = offered_mhz[i];
	return best;
}
