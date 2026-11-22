/* Finding a refresh rate the device can sustain (docs/lifecycle.md, "Stable rate"). The user's setting is a ceiling: the daemon starts at the best rate
 * the device offers under it and, if the stream is not healthy, steps down to the next rate the device offers until it is. Pure logic, so it is tested alone. */
#ifndef PDISP_RATESTEP_H
#define PDISP_RATESTEP_H

#include <stdbool.h>
#include <stdint.h>

#define RATESTEP_WINDOW 6        /* ticks (2 s each) looked at */
#define RATESTEP_MIN_SEEN 4      /* ticks of activity needed before the window can decide */
#define RATESTEP_SEVERE_IDR 3    /* keyframes forced by unresolved frames in one tick that make it severe */
#define RATESTEP_SEVERE_RUN 2    /* severe ticks in a row that step down at once (about 4 s) */
#define RATESTEP_STABLE_TICKS 10 /* ticks of activity without trouble (20 s) before the rate counts as found */

struct ratestep_sample {
	bool active;              /* frames were being sent in this tick (a static screen proves nothing) */
	unsigned idr_unresolved;  /* keyframes forced by unresolved frames in this tick */
	unsigned frames_lost;     /* frames the receiver reported lost in this tick */
	bool phone_slow;          /* the phone reported a decode time of 50 ms or more */
	bool low_rate;            /* the rate controller had to cut the target bitrate far down (the link or the receiver cannot take the stream) */
	bool heavy_repair;        /* repair packets were a large share of what was sent */
};

enum ratestep_action { RATESTEP_NONE, RATESTEP_DOWN, RATESTEP_STABLE };

struct ratestep {
	uint8_t bad[RATESTEP_WINDOW]; /* 1 = bad, 0 = good, 2 = no data */
	unsigned good_run, severe_run;
	bool stable_reported;
};

/* Forget the history: a session or a reconfiguration just started. */
void ratestep_reset(struct ratestep *r);
/* One tick. RATESTEP_DOWN: step to a lower rate now (the history is cleared): two severe ticks in a row (a storm of keyframes), or at least three
 * quarters of the ticks seen so far (four or more) bad. RATESTEP_STABLE: reported once, after enough healthy activity at this rate. */
enum ratestep_action ratestep_tick(struct ratestep *r, const struct ratestep_sample *s);

/* The next lower rate (millihertz) the device offers, at least `floor_mhz`; 0 if there is none. */
unsigned ratestep_next_lower(const uint32_t *offered_mhz, int n, unsigned current_mhz, unsigned floor_mhz);

#endif
