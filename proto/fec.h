/* Reed-Solomon erasure coding over GF(2^8) (polynomial 0x11d), systematic, Cauchy parity matrix.
 *
 * A block has `k` data shards (indices 0..k-1) and any number of parity shards (indices k..254), all of the same
 * length. Parity shard `s` is the sum over data shards `c` of inv(s xor c) * data[c], so every parity shard is
 * independent of how many others exist: more repair shards can be generated later for the same block. Any `k` distinct
 * shards of the block reconstruct all the data (every square submatrix of a Cauchy matrix is invertible).
 */
#ifndef PDISP_FEC_H
#define PDISP_FEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FEC_MAX_INDEX 254 /* highest shard index; with indices 0..254 a block has at most 255 shards */

/* Computes parity shard `index` (k <= index <= FEC_MAX_INDEX) from the k data shards into `out` (len bytes).
 * Returns 0, or -EINVAL for bad arguments. */
int fec_encode_shard(unsigned k, unsigned index, const uint8_t *const data[], size_t len, uint8_t *out);

/* Reconstructs the missing data shards. `shards[i]` for 0 <= i < n is shard i's buffer and `present[i]` tells whether it
 * holds received data. Every `shards[i]` with i < k must be a writable buffer of `len` bytes (missing data shards are
 * filled in place); parity shards are only read, and missing ones need no buffer. Returns 0 on success (including when
 * nothing is missing), -ENODATA if fewer than k shards are present, -EINVAL for bad arguments. */
int fec_reconstruct(unsigned k, unsigned n, uint8_t *const shards[], const bool present[], size_t len);

#endif
