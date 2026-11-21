/* Video packetisation and reassembly (docs/protocol.md section 3.2).
 *
 * Sender: a frame (one access unit) is cut into data shards of `shard_size` bytes; frames above 200 data shards are split
 * into several blocks. Each block gets Reed-Solomon parity. Any shard, including extra repair shards, is built on demand
 * from the frame bytes by vtx_packet(), so nothing but the frame itself needs to be kept to answer a NACK.
 *
 * Receiver: vrx_push() collects shards per frame and block, reconstructs a block as soon as any `data_shards` of its
 * shards have arrived, and hands out the frame when every block is complete. vrx_nacks() reports how many more shards each
 * incomplete block needs.
 *
 * Datagrams here are the plaintext common header + video header + shard (the AEAD tag, if any, is added and removed by the
 * layer below).
 */
#ifndef PDISP_VIDEO_H
#define PDISP_VIDEO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "proto.h"

#define VIDEO_MAX_SHARD_SIZE 1500
#define VIDEO_MAX_BLOCK_DATA 200 /* data shards per block; leaves at least 54 shard indices for parity and repairs */
#define VIDEO_NEED_ALL 255       /* NACK need value: nothing of this block was seen, so its size is unknown. The sender answers
                                  * with `data_shards` shards of its choice; for a large block that must include original data
                                  * shards, because only indices data+parity..254 are free for new repair parity. */

/* ---- sender */

struct vtx_frame {
	uint32_t frame_id, ref_frame_id;
	uint64_t capture_us;
	uint8_t epoch, vflags;
	const uint8_t *data; /* borrowed: must stay valid while packets are built */
	uint32_t size;
	unsigned shard_size;
	unsigned block_count;
	struct {
		uint32_t first_shard; /* index of the block's first data shard within the frame */
		uint8_t data_shards;
		uint8_t parity_shards; /* sent initially */
	} blocks[255];
};

/* Plans the blocks for a frame. `fec_percent` is the initial parity overhead (0 disables parity). Returns 0, or -EINVAL
 * (size 0, bad shard size) or -EMSGSIZE (frame needs more than 255 blocks). */
int vtx_frame_init(struct vtx_frame *f, uint32_t frame_id, uint32_t ref_frame_id, uint64_t capture_us, uint8_t epoch,
		   uint8_t vflags, const uint8_t *data, uint32_t size, unsigned shard_size, unsigned fec_percent);

/* Number of datagrams the frame sends initially (all data shards plus the initial parity). */
unsigned vtx_frame_packets(const struct vtx_frame *f);

/* Builds one datagram: shard `shard` of block `block`. Data shards are 0..data_shards-1; parity (initial or repair) is
 * data_shards..254. `pkt_flags` goes into the common header (use PROTO_PKT_FLAG_REPAIR for repairs). The datagram is
 * PROTO_PKT_HDR_SIZE + PROTO_VIDEO_HDR_SIZE + shard_size bytes. Returns its length, or a negative errno. */
int vtx_packet(const struct vtx_frame *f, uint32_t session_id, uint64_t seq, unsigned block, unsigned shard,
	       uint8_t pkt_flags, uint8_t *out, size_t cap);

/* ---- receiver */

struct vrx;

struct vrx_frame {
	uint32_t frame_id, ref_frame_id;
	uint64_t capture_us;
	uint8_t epoch, vflags;
	const uint8_t *data; /* valid until the next vrx_push() or vrx_destroy() */
	size_t size;
};

struct vrx_stats {
	uint64_t packets, duplicates, rejected, late; /* late: for a frame already complete or evicted */
	uint64_t frames_complete, frames_evicted;     /* evicted: dropped from the window while still incomplete */
	uint64_t shards_reconstructed;
};

struct vrx *vrx_create(unsigned shard_size);
void vrx_destroy(struct vrx *rx);
void vrx_set_epoch(struct vrx *rx, uint8_t epoch); /* drops everything buffered and accepts only this epoch */
const struct vrx_stats *vrx_stats(const struct vrx *rx);

/* Feeds one datagram. Returns 1 if it completed a frame (`*out` is filled), 0 if it was accepted or ignored, or a negative
 * errno if it was malformed or inconsistent with what is known about its frame. */
int vrx_push(struct vrx *rx, const uint8_t *pkt, size_t len, uint64_t now_us, struct vrx_frame *out);

/* Gives up on a frame: frees what was buffered for it and ignores its later packets. No-op for unknown frames. */
void vrx_drop_frame(struct vrx *rx, uint32_t frame_id);

/* Fills `out` with the blocks of incomplete frames that have been idle (no packet arrived) for at least `min_idle_us` and were not reported in
 * the last `retry_us`. `need` is the number of further shards required, or VIDEO_NEED_ALL for a block of which nothing
 * arrived. Returns the number of entries written (at most `max`). */
int vrx_nacks(struct vrx *rx, uint64_t now_us, uint64_t min_idle_us, uint64_t retry_us, struct proto_nack *out, int max);

#endif
