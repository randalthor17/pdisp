#include "inputseq.h"

#include <string.h>

void input_tx_init(struct input_tx *t)
{
	memset(t, 0, sizeof *t);
	t->next_seq = 1;
}

/* index of the oldest MOVE that a later event of the same pointer makes redundant, or -1 */
static int superseded_move(const struct input_tx *t)
{
	for (unsigned i = 0; i < t->n; i++) {
		if (t->q[i].kind != PROTO_IN_MOVE && t->q[i].kind != PROTO_IN_HOVER) continue;
		for (unsigned j = i + 1; j < t->n; j++)
			if (t->q[j].pointer_id == t->q[i].pointer_id) return (int)i;
	}
	return -1;
}

void input_tx_push(struct input_tx *t, struct proto_input_event e)
{
	if (t->n == PROTO_MAX_INPUT_EVENTS) {
		int k = superseded_move(t);
		unsigned drop = 0;
		if (k >= 0) { drop = (unsigned)k; t->coalesced++; }
		else { /* the PC has not heard from us for a long time: the oldest event goes, and the receiver is told */
			t->dropped++;
			if (!t->lost_upto || (int32_t)(t->q[0].seq - t->lost_upto) > 0) t->lost_upto = t->q[0].seq;
		}
		memmove(&t->q[drop], &t->q[drop + 1], (t->n - drop - 1) * sizeof t->q[0]);
		t->n--;
		if (t->fresh) t->fresh--; /* (it was either never sent, or the count only decides whether to send at once) */
	}
	e.seq = t->next_seq++;
	t->q[t->n++] = e;
	t->fresh++;
}

void input_tx_ack(struct input_tx *t, uint32_t acked)
{
	if ((int32_t)(acked - t->acked) > 0) t->acked = acked;
	unsigned keep = 0;
	while (keep < t->n && t->q[keep].seq <= acked) keep++;
	if (keep) {
		memmove(&t->q[0], &t->q[keep], (t->n - keep) * sizeof t->q[0]);
		t->n -= keep;
		t->fresh = t->fresh > t->n ? t->n : t->fresh;
	}
}

unsigned input_tx_pending(const struct input_tx *t) { return t->n; }

int input_tx_build(struct input_tx *t, uint64_t now_us, uint64_t resend_us, uint8_t *out, size_t cap)
{
	if (t->n == 0) return 0;
	if (!t->fresh && now_us - t->last_send_us < resend_us) return 0;
	struct proto_input in;
	memset(&in, 0, sizeof in);
	in.count = (uint8_t)t->n;
	memcpy(in.events, t->q, t->n * sizeof t->q[0]);
	if (t->lost_upto && (int32_t)(t->lost_upto - t->acked) > 0) { in.has_lost = 1; in.lost_upto = t->lost_upto; }
	int n = proto_input_encode(&in, out, cap);
	if (n < 0) return 0;
	t->last_send_us = now_us;
	t->fresh = 0;
	return n;
}

void input_rx_init(struct input_rx *r)
{
	memset(r, 0, sizeof *r);
	r->next_seq = 1;
}

unsigned input_rx_accept(struct input_rx *r, const struct proto_input *in, struct proto_input_event *out, int *gap)
{
	unsigned n = 0;
	/* events the sender reports as lost for good, that we have not got past: the fingers' state is unknown */
	*gap = in->has_lost && (int32_t)(in->lost_upto - r->next_seq) >= 0;
	if (*gap) r->gaps++;
	for (unsigned i = 0; i < in->count; i++) {
		const struct proto_input_event *e = &in->events[i];
		/* sequence numbers are compared as a signed difference so the counter may wrap; a skip is a superseded move or a reported loss */
		if ((int32_t)(e->seq - r->next_seq) < 0) { r->duplicates++; continue; }
		out[n++] = *e;
		r->next_seq = e->seq + 1;
	}
	if (*gap && (int32_t)(in->lost_upto - r->next_seq) >= 0) r->next_seq = in->lost_upto + 1; /* nothing newer than the loss arrived */
	return n;
}

uint32_t input_rx_acked(const struct input_rx *r) { return r->next_seq - 1; }
