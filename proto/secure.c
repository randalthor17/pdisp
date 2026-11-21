#include "secure.h"

#include <errno.h>
#include <string.h>

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint32_t get32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t get64(const uint8_t *p) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = v << 8 | p[i]; return v; }

void sec_ctl_init(struct sec_ctl *c, const uint8_t key[PC_KEY_LEN])
{
	memcpy(c->key, key, PC_KEY_LEN);
	c->n = 0;
}

static void ctl_nonce(uint8_t nonce[PC_NONCE_LEN], uint64_t n)
{
	memset(nonce, 0, 4);
	for (int i = 0; i < 8; i++) nonce[4 + i] = (uint8_t)(n >> (8 * i));
}

int sec_ctl_seal(struct sec_ctl *c, const uint8_t *plain, size_t len, uint8_t *out, size_t cap)
{
	if (len > SEC_CTL_MAX_PLAIN) return -EMSGSIZE;
	if (cap < 4 + len + PC_TAG_LEN) return -ENOSPC;
	uint8_t nonce[PC_NONCE_LEN];
	ctl_nonce(nonce, c->n++);
	put32(out, (uint32_t)(len + PC_TAG_LEN));
	pc_aead_seal(c->key, nonce, out, 4, plain, len, out + 4, out + 4 + len);
	return (int)(4 + len + PC_TAG_LEN);
}

int sec_ctl_open(struct sec_ctl *c, const uint8_t *buf, size_t len, uint8_t *out, size_t cap, size_t *plain_len)
{
	if (len < 4) return 0;
	uint32_t clen = get32(buf);
	if (clen < PC_TAG_LEN || clen > SEC_CTL_MAX_PLAIN + PC_TAG_LEN) return -EMSGSIZE;
	if (len < 4 + (size_t)clen) return 0;
	size_t n = clen - PC_TAG_LEN;
	if (n > cap) return -ENOSPC; /* the caller needs a bigger output buffer */
	uint8_t nonce[PC_NONCE_LEN];
	ctl_nonce(nonce, c->n);
	if (pc_aead_open(c->key, nonce, buf, 4, buf + 4, n, buf + 4 + n, out) != 0) return -EBADMSG;
	c->n++; /* only a frame that authenticated advances the counter */
	*plain_len = n;
	return (int)(4 + clen);
}

/* ---- datagrams */

void sec_udp_init(struct sec_udp *u, const uint8_t send_key[PC_KEY_LEN], const uint8_t recv_key[PC_KEY_LEN], int is_initiator)
{
	memset(u, 0, sizeof *u);
	memcpy(u->send_key, send_key, PC_KEY_LEN);
	memcpy(u->recv_key, recv_key, PC_KEY_LEN);
	u->send_dir = is_initiator ? 0 : 1;
	u->recv_dir = is_initiator ? 1 : 0;
}

static void udp_nonce(uint8_t nonce[PC_NONCE_LEN], uint8_t dir, const uint8_t *pkt)
{
	nonce[0] = dir;
	memset(nonce + 1, 0, 3);
	memcpy(nonce + 4, pkt + 8, 8); /* the 64-bit sequence number, as it is on the wire */
}

int sec_udp_seal(const struct sec_udp *u, uint8_t *pkt, size_t len, size_t cap)
{
	if (len < SEC_UDP_HEADER) return -EINVAL;
	if (cap < len + PC_TAG_LEN) return -ENOSPC;
	uint8_t nonce[PC_NONCE_LEN];
	udp_nonce(nonce, u->send_dir, pkt);
	pc_aead_seal(u->send_key, nonce, pkt, SEC_UDP_HEADER, pkt + SEC_UDP_HEADER, len - SEC_UDP_HEADER, pkt + SEC_UDP_HEADER,
		     pkt + len);
	return (int)(len + PC_TAG_LEN);
}

/* Is `seq` new? (does not record it) */
static int window_fresh(const struct sec_udp *u, uint64_t seq)
{
	if (!u->have_any || seq > u->highest) return 1;
	uint64_t back = u->highest - seq;
	if (back >= SEC_UDP_WINDOW) return 0;
	return !(u->window[back / 64] >> (back % 64) & 1);
}

static void window_record(struct sec_udp *u, uint64_t seq)
{
	if (!u->have_any) {
		memset(u->window, 0, sizeof u->window);
		u->highest = seq;
		u->window[0] = 1;
		u->have_any = 1;
		return;
	}
	if (seq > u->highest) {
		uint64_t shift = seq - u->highest;
		if (shift >= SEC_UDP_WINDOW) {
			memset(u->window, 0, sizeof u->window);
		} else { /* slide the bitmap towards older entries by `shift` bits */
			unsigned words = (unsigned)(shift / 64), bits = (unsigned)(shift % 64);
			for (int i = SEC_UDP_WINDOW / 64 - 1; i >= 0; i--) {
				uint64_t v = 0;
				if (i >= (int)words) {
					v = u->window[i - (int)words] << bits;
					if (bits && i - (int)words - 1 >= 0) v |= u->window[i - (int)words - 1] >> (64 - bits);
				}
				u->window[i] = v;
			}
		}
		u->highest = seq;
		u->window[0] |= 1;
	} else {
		uint64_t back = u->highest - seq;
		u->window[back / 64] |= 1ull << (back % 64);
	}
}

int sec_udp_open(struct sec_udp *u, uint8_t *pkt, size_t len)
{
	if (len < SEC_UDP_HEADER + PC_TAG_LEN) return -EINVAL;
	uint64_t seq = get64(pkt + 8);
	if (!window_fresh(u, seq)) return -EEXIST; /* cheap check first: a replayed packet costs no cryptography */
	size_t n = len - SEC_UDP_HEADER - PC_TAG_LEN;
	uint8_t nonce[PC_NONCE_LEN];
	udp_nonce(nonce, u->recv_dir, pkt);
	if (pc_aead_open(u->recv_key, nonce, pkt, SEC_UDP_HEADER, pkt + SEC_UDP_HEADER, n, pkt + SEC_UDP_HEADER + n, pkt + SEC_UDP_HEADER) != 0)
		return -EBADMSG;
	window_record(u, seq);
	return (int)(SEC_UDP_HEADER + n);
}
