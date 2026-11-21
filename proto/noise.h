/* Noise_XX_25519_ChaChaPoly_BLAKE2b handshake (https://noiseprotocol.org/noise.html), written over proto/crypto.h.
 *
 * The phone is the initiator, the PC the responder. Both have a static X25519 key pair that is their identity. Three messages
 *     -> e
 *     <- e, ee, s, es
 *     -> s, se
 * leave both sides with the other's authenticated static public key, forward-secret transport keys and a shared handshake hash.
 * Message payloads (the device names) are encrypted from the second message on. Nothing is trusted until noise_finish() succeeded
 * and the caller has decided whether it knows the peer's static key (identity.[ch]).
 */
#ifndef PDISP_NOISE_H
#define PDISP_NOISE_H

#include <stddef.h>
#include <stdint.h>

#include "crypto.h"

#define NOISE_INITIATOR 0
#define NOISE_RESPONDER 1
#define NOISE_MAX_PAYLOAD 128
#define NOISE_MAX_MSG (32 + 48 + NOISE_MAX_PAYLOAD + PC_TAG_LEN)

struct noise_hs {
	int role, step;
	uint8_t ck[PC_HASH_LEN], h[PC_HASH_LEN];
	uint8_t k[PC_KEY_LEN];
	uint64_t n;
	int has_key;
	uint8_t s[PC_DH_LEN], s_pub[PC_DH_LEN], e[PC_DH_LEN], e_pub[PC_DH_LEN], rs[PC_DH_LEN], re[PC_DH_LEN];
	int have_e;
};

struct noise_result {
	uint8_t ctl_send[PC_KEY_LEN], ctl_recv[PC_KEY_LEN]; /* keys for the control channel (the Noise transport ciphers) */
	uint8_t udp_send[PC_KEY_LEN], udp_recv[PC_KEY_LEN]; /* separate keys for the datagram channel, derived from the same chaining key */
	uint8_t hash[PC_HASH_LEN];                           /* handshake hash: binds everything both sides saw (for the pairing code) */
	uint8_t peer_static[PC_DH_LEN];                      /* the authenticated static public key of the other side */
};

/* `static_secret` is this side's identity key. `prologue` is mixed into the transcript: both sides must pass the same bytes. */
void noise_init(struct noise_hs *hs, int role, const uint8_t static_secret[PC_DH_LEN], const uint8_t *prologue, size_t prologue_len);
/* Test hook: use this ephemeral secret instead of a random one (call before the first noise_write / noise_read). */
void noise_set_ephemeral(struct noise_hs *hs, const uint8_t secret[PC_DH_LEN]);

/* Writes the next message with `payload` (up to NOISE_MAX_PAYLOAD bytes) into `out`. Returns its length or a negative errno. */
int noise_write(struct noise_hs *hs, const uint8_t *payload, size_t payload_len, uint8_t *out, size_t cap);
/* Reads the next message from the peer, returning the payload length (copied to `payload`) or a negative errno
 * (-EBADMSG: authentication failed, -EINVAL: malformed, -EPROTO: wrong order). After a failure the handshake is dead. */
int noise_read(struct noise_hs *hs, const uint8_t *msg, size_t len, uint8_t *payload, size_t payload_cap);

int noise_done(const struct noise_hs *hs); /* all three messages have been exchanged */
/* Derives the keys and wipes the handshake state. Returns 0, or -EPROTO if not done. */
int noise_finish(struct noise_hs *hs, struct noise_result *out);

/* A six-digit code from the handshake hash, shown on both screens so a person can compare them. */
uint32_t noise_sas(const uint8_t hash[PC_HASH_LEN]);

#endif
