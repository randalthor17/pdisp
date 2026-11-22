/* Video sender: pacing, repair, loss detection and rate adaptation (docs/protocol.md sections 3.2, 3.5, 3.6, 5).
 *
 * The caller feeds it encoded frames and a clock, and pulls ready-to-send datagrams (plaintext, without the AEAD tag).
 * Nothing here touches a socket or the encoder: the daemon wires `invalidate` to encoder_invalidate_from() and sends the
 * datagrams it returns.
 */
#ifndef PDISP_VSEND_H
#define PDISP_VSEND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "proto.h"

struct vsend_config {
	uint32_t session_id;
	uint8_t epoch;
	unsigned shard_size;           /* default 1200 */
	unsigned fec_percent_min;      /* adaptive FEC bounds; defaults 5 and 30 */
	unsigned fec_percent_max;
	uint32_t bitrate_kbps;         /* initial target */
	uint32_t min_bitrate_kbps, max_bitrate_kbps;
	unsigned pace_factor_percent;  /* send rate as a percentage of the target bitrate, default 150 */
	uint64_t keepalive_us;         /* KEEPALIVE after this long without video, default 100 ms */
	uint64_t repair_gap_us;        /* minimum time between repairs for the same block, default 8 ms */
	uint64_t pace_burst_us;        /* how far behind schedule the pacer may catch up in one go, default 2 ms */
	uint32_t max_resend_bytes;     /* largest frame resent in answer to a whole-frame NACK, default 64 KiB */
	uint64_t frame_interval_us;    /* 1e6 / refresh rate, default 16667: scales how many frames in flight count as a problem */
};

struct vsend_callbacks {
	/* The receiver gave up on this frame (and, by its own rule, on every frame that depended on it): call
	 * encoder_invalidate_frame(lost_frame). Called once per lost frame, oldest first. */
	void (*invalidate)(uint32_t lost_frame, void *user);
	/* the receiver's decoder broke: the next frame must be an IDR */
	void (*need_idr)(void *user);
	void *user;
};

struct vsend_stats {
	uint64_t frames, packets, bytes, repair_packets, keepalives;
	uint64_t nacks, frames_held, frames_lost, feedback;
	/* what the phone reported since the stats were last read (see vsend_take_report): its worst queue delay and decode time, and the lowest
	 * receive rate seen, for diagnosing why the bitrate fell */
	uint32_t fb_queue_max_us, fb_decode_max_x10, fb_recv_kbps_min;
};

struct vsend;

struct vsend *vsend_create(const struct vsend_config *cfg, const struct vsend_callbacks *cb);
void vsend_destroy(struct vsend *vs);

/* Queues an encoded frame for sending (the bytes are copied). Returns 0, -EAGAIN if the send queue is full (the caller
 * must not drop an encoded frame silently: it must report it as lost), or another negative errno. */
int vsend_submit(struct vsend *vs, uint32_t frame_id, uint32_t ref_frame_id, uint64_t capture_us, uint8_t vflags,
		 const uint8_t *data, uint32_t size, uint64_t now_us);

/* The next datagram that is due, or 0 if none is (yet). Call repeatedly until it returns 0. */
int vsend_next_packet(struct vsend *vs, uint64_t now_us, uint8_t *out, size_t cap);

/* When the next datagram will be due (now or later), or UINT64_MAX if the queue is empty. */
uint64_t vsend_next_due_us(const struct vsend *vs, uint64_t now_us);

/* Builds a datagram of any other type (header with the next sequence number, then `payload`), so everything the PC sends shares
 * one sequence space. Returns the length, or a negative errno. */
int vsend_make_packet(struct vsend *vs, uint8_t type, const uint8_t *payload, size_t len, uint8_t *out, size_t cap);

/* Processes a datagram from the phone; FEEDBACK packets are handled, others ignored. Returns 0, or a negative errno. */
int vsend_on_packet(struct vsend *vs, const uint8_t *pkt, size_t len, uint64_t now_us);

/* Backpressure for the encoder: bytes still waiting, and how long they take to send at the current pacing rate. The daemon
 * skips a capture before encoding when this is above about one frame interval. */
size_t vsend_queued_bytes(const struct vsend *vs);
uint64_t vsend_queue_delay_us(const struct vsend *vs);

/* The reference chain can no longer be trusted: more frames are unresolved (neither confirmed held nor reported lost) than a
 * healthy link would leave in flight, so the newest frames may all depend on a frame the receiver never got. The caller must
 * then encode an IDR. Returns false for 150 ms after an IDR, so one keyframe is not followed by a storm of them, and while the receiver's newest
 * frame id has not advanced for 100 ms, since a keyframe cannot get through a dead forward path. */
bool vsend_must_idr(const struct vsend *vs, uint64_t now_us);

/* Frames submitted but not yet resolved by the receiver, and the smoothed round trip (0 until measured). */
unsigned vsend_unresolved(const struct vsend *vs);
uint64_t vsend_rtt_us(const struct vsend *vs);

/* Rate adaptation output: the encoder's target bitrate and the FEC overhead to use for the next frames. */
uint32_t vsend_bitrate_kbps(const struct vsend *vs);
/* true while the phone reports a slow decode (50 ms or more): the sender then holds the rate instead of cutting it */
bool vsend_phone_slow(const struct vsend *vs);
unsigned vsend_fec_percent(const struct vsend *vs);

const struct vsend_stats *vsend_stats(const struct vsend *vs);
/* Starts a new window for the three feedback figures above. */
void vsend_reset_report(struct vsend *vs);

#endif
