/* The rate governor: a keyframe storm steps down within two ticks; steady trouble within four; a quiet screen proves nothing; a healthy stretch is reported once. */
#include <assert.h>
#include <stdio.h>

#include "../daemon/ratestep.h"

static struct ratestep_sample good = { .active = true };
static struct ratestep_sample bad = { .active = true, .idr_unresolved = 2 };
static struct ratestep_sample storm = { .active = true, .idr_unresolved = 3 };
static struct ratestep_sample idle = { .active = false, .idr_unresolved = 9 };

int main(void)
{
	struct ratestep r;
	/* a storm of keyframes: the second severe tick in a row decides */
	ratestep_reset(&r);
	assert(ratestep_tick(&r, &storm) == RATESTEP_NONE);
	assert(ratestep_tick(&r, &storm) == RATESTEP_DOWN);
	/* the history was cleared */
	assert(ratestep_tick(&r, &storm) == RATESTEP_NONE);
	/* a severe tick followed by a good one is not a run */
	ratestep_reset(&r);
	for (int i = 0; i < 10; i++) assert(ratestep_tick(&r, (i % 2) ? &good : &storm) == RATESTEP_NONE);

	/* steady trouble (not severe): three of the first four ticks bad */
	ratestep_reset(&r);
	assert(ratestep_tick(&r, &good) == RATESTEP_NONE);
	assert(ratestep_tick(&r, &bad) == RATESTEP_NONE);
	assert(ratestep_tick(&r, &bad) == RATESTEP_NONE);
	assert(ratestep_tick(&r, &bad) == RATESTEP_DOWN);
	/* all bad: needs four ticks to be sure */
	ratestep_reset(&r);
	for (int i = 0; i < 3; i++) assert(ratestep_tick(&r, &bad) == RATESTEP_NONE);
	assert(ratestep_tick(&r, &bad) == RATESTEP_DOWN);

	/* occasional trouble is tolerated (every other tick bad) */
	ratestep_reset(&r);
	for (int i = 0; i < 20; i++) assert(ratestep_tick(&r, (i % 2) ? &bad : &good) == RATESTEP_NONE);

	/* idle ticks count for nothing */
	ratestep_reset(&r);
	for (int i = 0; i < 40; i++) assert(ratestep_tick(&r, &idle) == RATESTEP_NONE);

	/* lost frames, a slow phone, a collapsed bitrate and a heavy repair load are trouble too */
	struct ratestep_sample lost = { .active = true, .frames_lost = 1 }, slow = { .active = true, .phone_slow = true };
	struct ratestep_sample lowr = { .active = true, .low_rate = true }, rep = { .active = true, .heavy_repair = true };
	struct ratestep_sample *kinds[] = { &lost, &slow, &lowr, &rep };
	for (int k = 0; k < 4; k++) {
		ratestep_reset(&r);
		for (int i = 0; i < 3; i++) assert(ratestep_tick(&r, kinds[k]) == RATESTEP_NONE);
		assert(ratestep_tick(&r, kinds[k]) == RATESTEP_DOWN);
	}

	/* stable once after 10 healthy ticks, and not again */
	ratestep_reset(&r);
	for (int i = 0; i < RATESTEP_STABLE_TICKS - 1; i++) assert(ratestep_tick(&r, &good) == RATESTEP_NONE);
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
