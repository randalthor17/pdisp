/* The reliable ordered input event stream (docs/protocol.md section 3.8), as two pure state machines with no sockets or clocks of their
 * own. Events are never retransmitted on request; every INPUT packet simply repeats all events the PC has not acknowledged yet, so a
 * lost packet is covered by the next one and input never waits behind video.
 *
 * Sender (the phone): push events as they happen, ask for the packet to send, feed acknowledgements back.
 * Receiver (the PC): hand it each INPUT packet; it returns the events to apply now, each exactly once and in order. */
#ifndef PDISP_INPUTSEQ_H
#define PDISP_INPUTSEQ_H

#include <stdint.h>

#include "proto.h"

struct input_tx {
	struct proto_input_event q[PROTO_MAX_INPUT_EVENTS]; /* unacknowledged, oldest first */
	unsigned n;
	uint32_t next_seq;      /* the sequence number the next pushed event gets (starts at 1) */
	uint64_t last_send_us;
	unsigned fresh;         /* events pushed since the last packet built */
	uint64_t dropped;       /* events discarded because the PC did not acknowledge for too long (should stay 0) */
	uint32_t lost_upto;     /* the highest sequence number of those; reported in every packet until it is acknowledged */
	uint32_t acked;
	uint64_t coalesced;     /* superseded move events discarded to make room */
};

void input_tx_init(struct input_tx *t);
/* Adds an event (its `seq` is assigned here). When 32 events are waiting, the oldest move that a later event of the same finger
 * supersedes is discarded first (harmless: the later move says where the finger is), so a finger-down or finger-up is only ever dropped
 * as a last resort, and that is then reported to the receiver. */
void input_tx_push(struct input_tx *t, struct proto_input_event e);
void input_tx_ack(struct input_tx *t, uint32_t acked);
/* Builds the INPUT payload to send now, or returns 0 when nothing is due: new events go out at once, unacknowledged ones again after
 * `resend_us`. Returns the payload length (it repeats every unacknowledged event). */
int input_tx_build(struct input_tx *t, uint64_t now_us, uint64_t resend_us, uint8_t *out, size_t cap);
unsigned input_tx_pending(const struct input_tx *t);

struct input_rx {
	uint32_t next_seq;      /* the next event expected (starts at 1) */
	uint64_t duplicates;    /* events seen again (the redundancy at work) */
	uint64_t gaps;          /* times events were lost for good (the sender gave up on them) */
};

void input_rx_init(struct input_rx *r);
/* Returns how many events of `in` are new (copied to `out`, which holds PROTO_MAX_INPUT_EVENTS), oldest first. Skipped sequence
 * numbers inside a stream are normal: the sender discards a move when a later move of the same finger replaces it. `*gap` is set only
 * when the packet reports (`has_lost`) that events were dropped which were not such moves and this receiver has not passed them: the
 * caller must then release every finger it believes is down BEFORE applying the returned events, since an up may be missing. */
unsigned input_rx_accept(struct input_rx *r, const struct proto_input *in, struct proto_input_event *out, int *gap);
/* The highest sequence number such that every event up to it has been applied: what INPUT_ACK carries. */
uint32_t input_rx_acked(const struct input_rx *r);

#endif
