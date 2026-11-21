#include "timesync.h"

#include <string.h>

void timesync_init(struct timesync *ts) { memset(ts, 0, sizeof *ts); }

void timesync_add(struct timesync *ts, uint64_t t1, uint64_t t2, uint64_t t3, uint64_t t4)
{
	int64_t a = (int64_t)t2 - (int64_t)t1, b = (int64_t)t3 - (int64_t)t4;
	int64_t rtt = ((int64_t)t4 - (int64_t)t1) - ((int64_t)t3 - (int64_t)t2);
	if (rtt < 0) return; /* impossible: a corrupt or reordered sample */
	ts->offset[ts->next] = (a + b) / 2;
	ts->rtt[ts->next] = rtt;
	ts->next = (ts->next + 1) % TIMESYNC_SAMPLES;
	if (ts->count < TIMESYNC_SAMPLES) ts->count++;
}

bool timesync_estimate(const struct timesync *ts, int64_t *offset_us, int64_t *rtt_us)
{
	if (!ts->count) return false;
	unsigned best = 0;
	for (unsigned i = 1; i < ts->count; i++)
		if (ts->rtt[i] < ts->rtt[best]) best = i;
	*offset_us = ts->offset[best];
	*rtt_us = ts->rtt[best];
	return true;
}
