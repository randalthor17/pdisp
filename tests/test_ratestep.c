/* The rate governor: trouble over most of a window steps down; a quiet screen proves nothing; a healthy stretch is reported once. */
#include <assert.h>
#include <stdio.h>

#include "../daemon/ratestep.h"

static struct ratestep_sample good = { .active = true };
static struct ratestep_sample bad = { .active = true, .idr_unresolved = 2 };
static struct ratestep_sample idle = { .active = false, .idr_unresolved = 9 };

int main(void)
{
	struct ratestep r;
	ratestep_reset(&r);
	/* trouble at once is not enough: a full window must be seen */
	for (int i = 0; i < 5; i++) assert(ratestep_tick(&r, &bad) == RATESTEP_NONE);
	assert(ratestep_tick(&r, &bad) == RATESTEP_DOWN);
	/* the history was cleared: it takes a whole new window */
	assert(ratestep_tick(&r, &bad) == RATESTEP_NONE);

	/* occasional trouble is tolerated (3 bad in 6) */
	ratestep_reset(&r);
	for (int i = 0; i < 12; i++) assert(ratestep_tick(&r, (i % 2) ? &bad : &good) == RATESTEP_NONE);

	/* 4 bad in 6 is not */
	ratestep_reset(&r);
	enum ratestep_action a = RATESTEP_NONE;
	for (int i = 0; i < 6; i++) a = ratestep_tick(&r, (i == 0 || i == 3) ? &good : &bad);
	assert(a == RATESTEP_DOWN);

	/* idle ticks count for nothing */
	ratestep_reset(&r);
	for (int i = 0; i < 40; i++) assert(ratestep_tick(&r, &idle) == RATESTEP_NONE);

	/* lost frames and a slow phone are trouble too */
	ratestep_reset(&r);
	struct ratestep_sample lost = { .active = true, .frames_lost = 1 }, slow = { .active = true, .phone_slow = true };
	for (int i = 0; i < 5; i++) assert(ratestep_tick(&r, (i & 1) ? &lost : &slow) == RATESTEP_NONE);
	assert(ratestep_tick(&r, &lost) == RATESTEP_DOWN);

	/* stable once after 15 healthy ticks, and not again */
	ratestep_reset(&r);
	for (int i = 0; i < 14; i++) assert(ratestep_tick(&r, &good) == RATESTEP_NONE);
	assert(ratestep_tick(&r, &good) == RATESTEP_STABLE);
	for (int i = 0; i < 30; i++) assert(ratestep_tick(&r, &good) == RATESTEP_NONE);

	/* the next lower rate the device offers */
	uint32_t offered[] = { 60000, 90000, 120000, 30000 };
	assert(ratestep_next_lower(offered, 4, 90000, 30000) == 60000);
	assert(ratestep_next_lower(offered, 4, 60000, 30000) == 30000);
	assert(ratestep_next_lower(offered, 4, 30000, 30000) == 0);
	assert(ratestep_next_lower(offered, 3, 60000, 30000) == 0);   /* 30000 is not in the first three */
	assert(ratestep_next_lower(offered, 4, 60000, 45000) == 0);   /* below the floor */
	puts("ratestep ok");
	return 0;
}
