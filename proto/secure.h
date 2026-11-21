/* Authenticated encryption for the two channels (docs/security.md), keyed from noise_finish().
 *
 * Control channel: each control frame (the plain u32 length, u16 type, u16 flags, payload) becomes one secure frame
 *     u32 ciphertext_length | ChaCha20-Poly1305(plain frame) | 16-byte tag
 * with the length bytes as associated data and a per-direction counter as the nonce. The counter makes dropped, repeated or
 * reordered frames fail authentication, which is right for a stream: the connection is simply dead.
 *
 * Datagrams: the 16-byte packet header stays readable (it is the associated data); everything after it is encrypted and a
 * 16-byte tag is appended. The nonce is a direction byte and the header's 64-bit sequence number, which never repeats within a
 * key. The receiver keeps a sliding window of sequence numbers and drops replays; losing and reordering packets is normal here.
 */
#ifndef PDISP_SECURE_H
#define PDISP_SECURE_H

#include <stddef.h>
#include <stdint.h>

#include "crypto.h"

#define SEC_CTL_MAX_PLAIN (1u << 20)
#define SEC_UDP_OVERHEAD PC_TAG_LEN
#define SEC_UDP_HEADER 16
#define SEC_UDP_WINDOW 1024

/* ---- control channel */

struct sec_ctl {
	uint8_t key[PC_KEY_LEN];
	uint64_t n;
};

void sec_ctl_init(struct sec_ctl *c, const uint8_t key[PC_KEY_LEN]);
/* Seals one plain frame into `out`; returns the secure frame's length, or -EMSGSIZE / -ENOSPC. */
int sec_ctl_seal(struct sec_ctl *c, const uint8_t *plain, size_t len, uint8_t *out, size_t cap);
/* Looks at the start of `buf` for one complete secure frame. Returns 0 if more bytes are needed, -EBADMSG if authentication
 * failed or -EMSGSIZE if the length is not acceptable (the connection must be dropped for both), -ENOSPC if `out` is too small
 * for this frame (retry with more room), or the number of bytes
 * consumed, with the plain frame in `out` (`*plain_len` bytes). */
int sec_ctl_open(struct sec_ctl *c, const uint8_t *buf, size_t len, uint8_t *out, size_t cap, size_t *plain_len);

/* ---- datagrams */

struct sec_udp {
	uint8_t send_key[PC_KEY_LEN], recv_key[PC_KEY_LEN];
	uint8_t send_dir, recv_dir; /* 0: initiator (phone) to responder (PC), 1: the other way */
	uint64_t highest;
	uint64_t window[SEC_UDP_WINDOW / 64];
	int have_any;
};

void sec_udp_init(struct sec_udp *u, const uint8_t send_key[PC_KEY_LEN], const uint8_t recv_key[PC_KEY_LEN], int is_initiator);
/* In place: `pkt` holds `len` bytes (16-byte header plus payload) and has room for SEC_UDP_OVERHEAD more. Returns the new length,
 * or a negative errno. Needs no state, so it may be called from any thread. */
int sec_udp_seal(const struct sec_udp *u, uint8_t *pkt, size_t len, size_t cap);
/* In place: returns the length of header plus payload, or -EINVAL (too short), -EBADMSG (authentication failed) or -EEXIST (a
 * sequence number already seen, or older than the window). The window moves only for packets that authenticated. */
int sec_udp_open(struct sec_udp *u, uint8_t *pkt, size_t len);

#endif
