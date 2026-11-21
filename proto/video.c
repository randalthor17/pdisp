#include "video.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "fec.h"

/* ---- sender */

int vtx_frame_init(struct vtx_frame *f, uint32_t frame_id, uint32_t ref_frame_id, uint64_t capture_us, uint8_t epoch,
		   uint8_t vflags, const uint8_t *data, uint32_t size, unsigned shard_size, unsigned fec_percent)
{
	if (size == 0 || !data || shard_size < 64 || shard_size > VIDEO_MAX_SHARD_SIZE || fec_percent > 100) return -EINVAL;
	unsigned total = (size + shard_size - 1) / shard_size;
	unsigned blocks = (total + VIDEO_MAX_BLOCK_DATA - 1) / VIDEO_MAX_BLOCK_DATA;
	if (blocks > 255) return -EMSGSIZE;

	memset(f, 0, sizeof *f);
	f->frame_id = frame_id;
	f->ref_frame_id = ref_frame_id;
	f->capture_us = capture_us;
	f->epoch = epoch;
	f->vflags = vflags;
	f->data = data;
	f->size = size;
	f->shard_size = shard_size;
	f->block_count = blocks;

	unsigned base = total / blocks, extra = total % blocks, first = 0;
	for (unsigned b = 0; b < blocks; b++) {
		unsigned k = base + (b < extra ? 1 : 0);
		unsigned parity = fec_percent ? (k * fec_percent + 99) / 100 : 0;
		if (fec_percent && parity < 1) parity = 1;
		if (k + parity > FEC_MAX_INDEX + 1u) parity = FEC_MAX_INDEX + 1u - k;
		f->blocks[b].first_shard = first;
		f->blocks[b].data_shards = (uint8_t)k;
		f->blocks[b].parity_shards = (uint8_t)parity;
		first += k;
	}
	return 0;
}

unsigned vtx_frame_packets(const struct vtx_frame *f)
{
	unsigned n = 0;
	for (unsigned b = 0; b < f->block_count; b++) n += f->blocks[b].data_shards + f->blocks[b].parity_shards;
	return n;
}

/* Copies data shard `index` of the frame into `dst` (shard_size bytes), zero-padding the last one. */
static void load_data_shard(const struct vtx_frame *f, unsigned index, uint8_t *dst)
{
	size_t off = (size_t)index * f->shard_size;
	size_t n = off < f->size ? f->size - off : 0;
	if (n > f->shard_size) n = f->shard_size;
	if (n) memcpy(dst, f->data + off, n);
	if (n < f->shard_size) memset(dst + n, 0, f->shard_size - n);
}

int vtx_packet(const struct vtx_frame *f, uint32_t session_id, uint64_t seq, unsigned block, unsigned shard,
	       uint8_t pkt_flags, uint8_t *out, size_t cap)
{
	if (block >= f->block_count || shard > FEC_MAX_INDEX) return -EINVAL;
	const unsigned k = f->blocks[block].data_shards;
	const size_t total = PROTO_PKT_HDR_SIZE + PROTO_VIDEO_HDR_SIZE + f->shard_size;
	if (cap < total) return -EMSGSIZE;

	struct proto_pkt_hdr ph = { .session_id = session_id, .type = PROTO_PKT_VIDEO, .flags = pkt_flags, .seq = seq };
	struct proto_video_hdr vh = { .frame_id = f->frame_id, .ref_frame_id = f->ref_frame_id, .capture_us = f->capture_us,
				      .frame_bytes = f->size, .epoch = f->epoch, .block_index = (uint8_t)block,
				      .block_count = (uint8_t)f->block_count, .shard_index = (uint8_t)shard,
				      .data_shards = (uint8_t)k, .parity_shards = f->blocks[block].parity_shards,
				      .vflags = f->vflags };
	int rc = proto_pkt_hdr_write(&ph, out, cap);
	if (rc < 0) return rc;
	rc = proto_video_hdr_write(&vh, out + PROTO_PKT_HDR_SIZE, cap - PROTO_PKT_HDR_SIZE);
	if (rc < 0) return rc;
	uint8_t *payload = out + PROTO_PKT_HDR_SIZE + PROTO_VIDEO_HDR_SIZE;

	const unsigned first = f->blocks[block].first_shard;
	if (shard < k) {
		load_data_shard(f, first + shard, payload);
		return (int)total;
	}

	/* parity: every data shard of the block, the last one of the frame zero-padded */
	uint8_t padded[VIDEO_MAX_SHARD_SIZE];
	const uint8_t *ptrs[VIDEO_MAX_BLOCK_DATA];
	for (unsigned c = 0; c < k; c++) {
		size_t off = (size_t)(first + c) * f->shard_size;
		if (off + f->shard_size <= f->size) {
			ptrs[c] = f->data + off;
		} else {
			load_data_shard(f, first + c, padded);
			ptrs[c] = padded;
		}
	}
	rc = fec_encode_shard(k, shard, ptrs, f->shard_size, payload);
	return rc < 0 ? rc : (int)total;
}

/* ---- receiver */

#define VRX_WINDOW 64
#define VRX_MAX_FRAME (64u << 20)
#define VRX_MAX_BUFFERED (128u << 20) /* shard bytes held across all incomplete frames */

struct vrx_block {
	bool seen;
	bool complete;
	bool nacked;
	uint8_t data_shards, parity_shards;
	unsigned received;
	uint8_t **shard; /* 255 entries, allocated once the block is first seen */
	uint64_t last_nack_us;
};

struct vrx_slot {
	bool used, complete;
	uint32_t frame_id, ref_frame_id, frame_bytes;
	uint64_t capture_us, first_seen_us, last_packet_us;
	uint8_t epoch, vflags, block_count;
	struct vrx_block *blocks;
};

struct vrx {
	unsigned shard_size;
	uint8_t epoch;
	struct vrx_slot slots[VRX_WINDOW];
	struct vrx_stats st;
	size_t buffered; /* shard bytes currently held */
	uint8_t *out;
	size_t out_cap;
};

static void block_free_shards(struct vrx *rx, struct vrx_block *b)
{
	if (!b->shard) return;
	for (int i = 0; i < 255; i++)
		if (b->shard[i]) {
			free(b->shard[i]);
			rx->buffered -= rx->shard_size;
		}
	free(b->shard);
	b->shard = NULL;
}

static void slot_clear(struct vrx *rx, struct vrx_slot *s)
{
	if (s->blocks) {
		for (unsigned i = 0; i < s->block_count; i++) block_free_shards(rx, &s->blocks[i]);
		free(s->blocks);
	}
	memset(s, 0, sizeof *s);
}

struct vrx *vrx_create(unsigned shard_size)
{
	if (shard_size < 64 || shard_size > VIDEO_MAX_SHARD_SIZE) return NULL;
	struct vrx *rx = calloc(1, sizeof *rx);
	if (rx) rx->shard_size = shard_size;
	return rx;
}

void vrx_destroy(struct vrx *rx)
{
	if (!rx) return;
	for (int i = 0; i < VRX_WINDOW; i++) slot_clear(rx, &rx->slots[i]);
	free(rx->out);
	free(rx);
}

void vrx_set_epoch(struct vrx *rx, uint8_t epoch)
{
	for (int i = 0; i < VRX_WINDOW; i++) slot_clear(rx, &rx->slots[i]);
	rx->epoch = epoch;
}

const struct vrx_stats *vrx_stats(const struct vrx *rx) { return &rx->st; }

/* Reconstructs a block once enough shards are present. */
static int block_reconstruct(struct vrx *rx, struct vrx_block *b)
{
	const unsigned k = b->data_shards, s = rx->shard_size;
	uint8_t *ptrs[255];
	bool present[255];
	unsigned n = k, missing = 0;
	for (unsigned i = 0; i < 255; i++)
		if (b->shard[i] && i + 1 > n) n = i + 1;
	for (unsigned i = 0; i < n; i++) {
		present[i] = b->shard[i] != NULL;
		ptrs[i] = b->shard[i];
		if (i < k && !present[i]) {
			ptrs[i] = malloc(s);
			if (!ptrs[i]) {
				for (unsigned j = 0; j < i; j++)
					if (j < k && !present[j]) free(ptrs[j]);
				return -ENOMEM;
			}
			missing++;
		}
	}
	int rc = missing ? fec_reconstruct(k, n, ptrs, present, s) : 0;
	for (unsigned i = 0; i < k; i++) {
		if (present[i]) continue;
		if (rc == 0) b->shard[i] = ptrs[i]; else free(ptrs[i]);
	}
	if (rc != 0) return rc;
	rx->st.shards_reconstructed += missing;
	b->complete = true;
	for (unsigned i = k; i < 255; i++) /* parity no longer needed */
		if (b->shard[i]) { free(b->shard[i]); b->shard[i] = NULL; rx->buffered -= s; }
	rx->buffered += (size_t)missing * s; /* the recovered data shards are now held too */
	return 0;
}

int vrx_push(struct vrx *rx, const uint8_t *pkt, size_t len, uint64_t now_us, struct vrx_frame *out)
{
	const size_t want = PROTO_PKT_HDR_SIZE + PROTO_VIDEO_HDR_SIZE + rx->shard_size;
	struct proto_pkt_hdr ph;
	struct proto_video_hdr vh;
	if (len != want || proto_pkt_hdr_read(pkt, len, &ph) != 0 || ph.type != PROTO_PKT_VIDEO ||
	    proto_video_hdr_read(pkt + PROTO_PKT_HDR_SIZE, len - PROTO_PKT_HDR_SIZE, &vh) != 0 ||
	    vh.frame_bytes == 0 || vh.frame_bytes > VRX_MAX_FRAME) {
		rx->st.rejected++;
		return -EINVAL;
	}
	const uint8_t *payload = pkt + PROTO_PKT_HDR_SIZE + PROTO_VIDEO_HDR_SIZE;
	rx->st.packets++;

	if (vh.epoch != rx->epoch) { rx->st.late++; return 0; }

	struct vrx_slot *slot = &rx->slots[vh.frame_id % VRX_WINDOW];
	if (slot->used && slot->frame_id != vh.frame_id) {
		if ((int32_t)(vh.frame_id - slot->frame_id) < 0) { rx->st.late++; return 0; } /* older than what the slot holds */
		if (!slot->complete) rx->st.frames_evicted++;
		slot_clear(rx, slot);
	}
	if (slot->used && slot->complete) { rx->st.late++; return 0; }
	if (slot->used) slot->last_packet_us = now_us;

	if (!slot->used) {
		slot->blocks = calloc(vh.block_count, sizeof *slot->blocks);
		if (!slot->blocks) return -ENOMEM;
		slot->used = true;
		slot->frame_id = vh.frame_id;
		slot->ref_frame_id = vh.ref_frame_id;
		slot->capture_us = vh.capture_us;
		slot->frame_bytes = vh.frame_bytes;
		slot->epoch = vh.epoch;
		slot->vflags = vh.vflags;
		slot->block_count = vh.block_count;
		slot->first_seen_us = now_us;
		slot->last_packet_us = now_us;
	} else if (slot->ref_frame_id != vh.ref_frame_id || slot->capture_us != vh.capture_us ||
		   slot->frame_bytes != vh.frame_bytes || slot->block_count != vh.block_count || slot->vflags != vh.vflags) {
		rx->st.rejected++;
		return -EPROTO; /* contradicts what an earlier packet of this frame said */
	}

	struct vrx_block *b = &slot->blocks[vh.block_index];
	if (!b->seen) {
		b->shard = calloc(255, sizeof *b->shard);
		if (!b->shard) return -ENOMEM;
		b->seen = true;
		b->data_shards = vh.data_shards;
		b->parity_shards = vh.parity_shards;
	} else if (b->data_shards != vh.data_shards || b->parity_shards != vh.parity_shards) {
		rx->st.rejected++;
		return -EPROTO;
	}
	if (b->complete || b->shard[vh.shard_index]) { rx->st.duplicates++; return 0; }

	if (rx->buffered + rx->shard_size > VRX_MAX_BUFFERED) { rx->st.rejected++; return -ENOMEM; }
	b->shard[vh.shard_index] = malloc(rx->shard_size);
	if (!b->shard[vh.shard_index]) return -ENOMEM;
	memcpy(b->shard[vh.shard_index], payload, rx->shard_size);
	rx->buffered += rx->shard_size;
	b->received++;

	if (b->received < b->data_shards) return 0;
	int rc = block_reconstruct(rx, b);
	if (rc != 0) return rc;

	/* the frame is done when every block is */
	size_t data_shards = 0;
	for (unsigned i = 0; i < slot->block_count; i++) {
		if (!slot->blocks[i].complete) return 0;
		data_shards += slot->blocks[i].data_shards;
	}
	if ((data_shards - 1) * rx->shard_size >= slot->frame_bytes || data_shards * rx->shard_size < slot->frame_bytes) {
		/* the blocks do not add up to the announced size: a corrupt or hostile frame */
		rx->st.rejected++;
		rx->st.frames_evicted++;
		slot_clear(rx, slot);
		return -EPROTO;
	}
	if (rx->out_cap < slot->frame_bytes) {
		uint8_t *p = realloc(rx->out, slot->frame_bytes);
		if (!p) return -ENOMEM;
		rx->out = p;
		rx->out_cap = slot->frame_bytes;
	}
	size_t off = 0;
	for (unsigned i = 0; i < slot->block_count; i++)
		for (unsigned c = 0; c < slot->blocks[i].data_shards; c++) {
			size_t n = slot->frame_bytes - off;
			if (n > rx->shard_size) n = rx->shard_size;
			memcpy(rx->out + off, slot->blocks[i].shard[c], n);
			off += n;
		}
	out->frame_id = slot->frame_id;
	out->ref_frame_id = slot->ref_frame_id;
	out->capture_us = slot->capture_us;
	out->epoch = slot->epoch;
	out->vflags = slot->vflags;
	out->data = rx->out;
	out->size = slot->frame_bytes;

	/* keep the slot so late and duplicate packets of this frame are recognised, but drop the buffered shards */
	for (unsigned i = 0; i < slot->block_count; i++) block_free_shards(rx, &slot->blocks[i]);
	slot->complete = true;
	rx->st.frames_complete++;
	return 1;
}

void vrx_drop_frame(struct vrx *rx, uint32_t frame_id)
{
	struct vrx_slot *s = &rx->slots[frame_id % VRX_WINDOW];
	if (!s->used || s->frame_id != frame_id || s->complete) return;
	for (unsigned i = 0; i < s->block_count; i++) block_free_shards(rx, &s->blocks[i]);
	s->complete = true; /* late and duplicate packets are now ignored, and no NACKs are made for it */
	rx->st.frames_evicted++;
}

struct nack_cand {
	struct proto_nack nack;
	struct vrx_block *blk;
};

static int nack_cmp(const void *a, const void *b)
{
	const struct nack_cand *x = a, *y = b;
	if (x->nack.frame_id != y->nack.frame_id) return (int32_t)(x->nack.frame_id - y->nack.frame_id) < 0 ? -1 : 1;
	return (int)x->nack.block_index - (int)y->nack.block_index;
}

int vrx_nacks(struct vrx *rx, uint64_t now_us, uint64_t min_idle_us, uint64_t retry_us, struct proto_nack *out, int max)
{
	struct nack_cand all[VRX_WINDOW * 4];
	int n = 0;
	for (int i = 0; i < VRX_WINDOW; i++) {
		struct vrx_slot *s = &rx->slots[i];
		if (!s->used || s->complete || now_us < s->last_packet_us + min_idle_us) continue;
		for (unsigned j = 0; j < s->block_count && n < (int)(sizeof all / sizeof *all); j++) {
			struct vrx_block *b = &s->blocks[j];
			if (b->complete || (b->nacked && now_us < b->last_nack_us + retry_us)) continue;
			all[n].nack.frame_id = s->frame_id;
			all[n].nack.block_index = (uint8_t)j;
			all[n].nack.need = b->seen ? (uint8_t)(b->data_shards - b->received) : VIDEO_NEED_ALL;
			all[n].blk = b;
			n++;
		}
	}
	qsort(all, (size_t)n, sizeof *all, nack_cmp);
	if (n > max) n = max;
	for (int i = 0; i < n; i++) { /* only what is reported counts as reported */
		out[i] = all[i].nack;
		all[i].blk->nacked = true;
		all[i].blk->last_nack_us = now_us;
	}
	return n;
}
