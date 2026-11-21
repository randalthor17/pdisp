/* Video receiver: reassembly, frame gating, loss reporting (docs/protocol.md sections 3.3, 3.5, 3.6).
 *
 * Gating rule: a completed frame goes to the decoder only if it is an IDR or the frame it predicts from was decoded. A frame
 * whose reference is still in flight waits briefly; a frame whose reference was given up is given up too. The last good
 * picture stays on screen meanwhile (a freeze, never garbage). Every frame ends up resolved, either decoded or given up,
 * and the sender learns which from the feedback.
 */
#ifndef PDISP_VRECV_H
#define PDISP_VRECV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "proto.h"

struct vrecv_config {
	uint32_t session_id;
	unsigned shard_size;        /* default 1200 */
	uint64_t giveup_us;         /* give up on a frame this long after first hearing of it, default 100 ms */
	uint64_t nack_min_age_us;   /* ask for repairs once a frame has been idle this long (no packet), default 4 ms */
	uint64_t nack_retry_us;     /* ask again after this long, default 15 ms */
	unsigned max_pending;       /* completed frames kept waiting for their reference, default 8 */
};

struct vrecv_frame {
	uint32_t frame_id, ref_frame_id;
	uint64_t capture_us;
	uint8_t vflags;
	const uint8_t *data; /* valid only during the decode callback */
	size_t size;
};

struct vrecv_callbacks {
	/* Decode and present this frame. Return 0 on success; non-zero means the decoder failed and an IDR is needed. */
	int (*decode)(const struct vrecv_frame *frame, void *user);
	void *user;
};

struct vrecv_stats {
	uint64_t packets, frames_decoded, frames_given_up, idr_decoded, decoder_errors;
};

struct vrecv;

struct vrecv *vrecv_create(const struct vrecv_config *cfg, const struct vrecv_callbacks *cb);
void vrecv_destroy(struct vrecv *vr);
void vrecv_set_epoch(struct vrecv *vr, uint8_t epoch);

/* Processes a datagram from the PC (VIDEO and KEEPALIVE are used). May invoke the decode callback. */
int vrecv_on_packet(struct vrecv *vr, const uint8_t *pkt, size_t len, uint64_t now_us);

/* Gives up on frames that waited too long. Call regularly (at least every few milliseconds). */
void vrecv_tick(struct vrecv *vr, uint64_t now_us);

/* Builds a complete FEEDBACK datagram (common header + payload). Returns its length, or 0 if there is nothing to report
 * yet. Call about every 10 ms and after anything notable. */
int vrecv_feedback_packet(struct vrecv *vr, uint64_t now_us, uint8_t *out, size_t cap);

/* The platform decoder reported an error outside the decode callback: ask for an IDR. */
void vrecv_decoder_error(struct vrecv *vr);

const struct vrecv_stats *vrecv_stats(const struct vrecv *vr);

#endif
