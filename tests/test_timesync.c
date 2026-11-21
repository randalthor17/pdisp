/* Tests for the clock offset estimator. */
#include "timesync.h"

#include <stdio.h>

static int failures;
#define CHECK(cond)                                                                                  \
	do {                                                                                         \
		if (!(cond)) {                                                                       \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);     \
			failures++;                                                                  \
		}                                                                                    \
	} while (0)

static int64_t llabs64(int64_t v) { return v < 0 ? -v : v; }

/* one exchange with the server clock running `offset` ahead, and the given one-way delays and server turnaround */
static void exchange(struct timesync *ts, uint64_t client_now, int64_t offset, uint64_t d1, uint64_t d2, uint64_t turn)
{
	uint64_t t1 = client_now;
	uint64_t t2 = (uint64_t)((int64_t)(t1 + d1) + offset);
	uint64_t t3 = t2 + turn;
	uint64_t t4 = t1 + d1 + turn + d2;
	timesync_add(ts, t1, t2, t3, t4);
}

int main(void)
{
	struct timesync ts;
	int64_t off, rtt;

	timesync_init(&ts);
	CHECK(!timesync_estimate(&ts, &off, &rtt));

	/* symmetric delays: the offset is exact, whatever its size or sign */
	for (int64_t k = -1000000000000ll; k <= 1000000000000ll; k += 1000000000000ll) {
		timesync_init(&ts);
		exchange(&ts, 5000000, k, 8000, 8000, 50);
		CHECK(timesync_estimate(&ts, &off, &rtt) && off == k && rtt == 16000);
	}

	/* asymmetry d1 - d2 = 6 ms leaves an error of half that */
	timesync_init(&ts);
	exchange(&ts, 1000000, 123456, 11000, 5000, 0);
	CHECK(timesync_estimate(&ts, &off, &rtt));
	CHECK(llabs64(off - 123456) == 3000 && rtt == 16000);

	/* the sample with the lowest round trip wins: queued exchanges (long, lopsided) do not spoil the estimate */
	timesync_init(&ts);
	exchange(&ts, 1000000, 777, 40000, 9000, 0);  /* queued one way */
	exchange(&ts, 2000000, 777, 6000, 6100, 0);   /* clean */
	exchange(&ts, 3000000, 777, 7000, 55000, 0);  /* queued the other way */
	CHECK(timesync_estimate(&ts, &off, &rtt) && llabs64(off - 777) <= 50 && rtt == 12100);

	/* only the last TIMESYNC_SAMPLES exchanges count: an old clean sample ages out (clocks drift) */
	timesync_init(&ts);
	exchange(&ts, 1000000, 100, 5000, 5000, 0);
	for (int i = 0; i < TIMESYNC_SAMPLES; i++) exchange(&ts, 2000000 + (uint64_t)i * 1000000, 900, 9000, 9000, 0);
	CHECK(timesync_estimate(&ts, &off, &rtt) && off == 900);

	/* an impossible sample (the answer came back before it could have been sent) is ignored */
	timesync_init(&ts);
	timesync_add(&ts, 1000, 5000, 9000, 2000);
	CHECK(!timesync_estimate(&ts, &off, &rtt));

	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	puts("timesync: all checks passed");
	return 0;
}
