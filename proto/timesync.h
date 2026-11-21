/* NTP-style clock offset estimation between the phone ("client") and the PC ("server"), docs/protocol.md section 3.9.
 *
 * One exchange: the client sends TIME_REQ at t1 (its clock); the server receives it at t2 and answers at t3 (its clock);
 * the client receives the answer at t4 (its clock). Then offset = ((t2 - t1) + (t3 - t4)) / 2 is how far the server clock is
 * ahead of the client's, and round trip = (t4 - t1) - (t3 - t2). Path asymmetry is the only error, at most half the
 * difference between the two one-way delays. The sample with the lowest round trip among the last few is the most trustworthy
 * (it queued the least), so that one is used.
 */
#ifndef PDISP_TIMESYNC_H
#define PDISP_TIMESYNC_H

#include <stdbool.h>
#include <stdint.h>

#define TIMESYNC_SAMPLES 10

struct timesync {
	int64_t offset[TIMESYNC_SAMPLES];
	int64_t rtt[TIMESYNC_SAMPLES];
	unsigned count, next;
};

void timesync_init(struct timesync *ts);
void timesync_add(struct timesync *ts, uint64_t t1, uint64_t t2, uint64_t t3, uint64_t t4);
/* The current estimate: `*offset_us` such that server_time = client_time + offset. False until a sample exists. */
bool timesync_estimate(const struct timesync *ts, int64_t *offset_us, int64_t *rtt_us);

#endif
