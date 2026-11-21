#include "noise.h"

#include <errno.h>
#include <string.h>

static const char protocol_name[] = "Noise_XX_25519_ChaChaPoly_BLAKE2b";

static void nonce_of(uint8_t nonce[12], uint64_t n)
{
	memset(nonce, 0, 4);
	for (int i = 0; i < 8; i++) nonce[4 + i] = (uint8_t)(n >> (8 * i));
}

static void mix_hash(struct noise_hs *hs, const uint8_t *data, size_t len) { pc_hash2(hs->h, hs->h, PC_HASH_LEN, data, len); }

static void mix_key(struct noise_hs *hs, const uint8_t ikm[PC_DH_LEN])
{
	uint8_t out1[PC_HASH_LEN], out2[PC_HASH_LEN];
	pc_hkdf(hs->ck, ikm, PC_DH_LEN, out1, out2, NULL);
	memcpy(hs->ck, out1, PC_HASH_LEN);
	memcpy(hs->k, out2, PC_KEY_LEN); /* the first 32 bytes of the second output */
	hs->n = 0;
	hs->has_key = 1;
	pc_wipe(out1, sizeof out1);
	pc_wipe(out2, sizeof out2);
}

/* EncryptAndHash: with a key the output is ciphertext plus tag, without one it is the input unchanged. Returns the output length. */
static size_t encrypt_and_hash(struct noise_hs *hs, const uint8_t *in, size_t len, uint8_t *out)
{
	size_t n = len;
	if (hs->has_key) {
		uint8_t nonce[12];
		nonce_of(nonce, hs->n++);
		pc_aead_seal(hs->k, nonce, hs->h, PC_HASH_LEN, in, len, out, out + len);
		n = len + PC_TAG_LEN;
	} else if (len) {
		memmove(out, in, len);
	}
	mix_hash(hs, out, n);
	return n;
}

/* DecryptAndHash: returns the plaintext length, or -EBADMSG. */
static int decrypt_and_hash(struct noise_hs *hs, const uint8_t *in, size_t len, uint8_t *out)
{
	size_t plain = len;
	if (hs->has_key) {
		if (len < PC_TAG_LEN) return -EINVAL;
		plain = len - PC_TAG_LEN;
		uint8_t nonce[12];
		nonce_of(nonce, hs->n);
		if (pc_aead_open(hs->k, nonce, hs->h, PC_HASH_LEN, in, plain, in + plain, out) != 0) return -EBADMSG;
		hs->n++;
	} else if (len) {
		memmove(out, in, len);
	}
	mix_hash(hs, in, len);
	return (int)plain;
}

void noise_init(struct noise_hs *hs, int role, const uint8_t static_secret[PC_DH_LEN], const uint8_t *prologue, size_t prologue_len)
{
	memset(hs, 0, sizeof *hs);
	hs->role = role;
	memcpy(hs->s, static_secret, PC_DH_LEN);
	pc_x25519_public(hs->s_pub, hs->s);
	/* the protocol name is shorter than the hash: it is padded with zeros and used as the initial hash */
	memcpy(hs->h, protocol_name, sizeof protocol_name - 1);
	memcpy(hs->ck, hs->h, PC_HASH_LEN);
	mix_hash(hs, prologue, prologue_len);
}

void noise_set_ephemeral(struct noise_hs *hs, const uint8_t secret[PC_DH_LEN])
{
	memcpy(hs->e, secret, PC_DH_LEN);
	pc_x25519_public(hs->e_pub, hs->e);
	hs->have_e = 1;
}

static int make_ephemeral(struct noise_hs *hs)
{
	if (!hs->have_e) {
		if (pc_random(hs->e, PC_DH_LEN) != 0) return -EIO;
		pc_x25519_public(hs->e_pub, hs->e);
		hs->have_e = 1;
	}
	return 0;
}

static int dh_mix(struct noise_hs *hs, const uint8_t secret[PC_DH_LEN], const uint8_t pub[PC_DH_LEN])
{
	uint8_t dh[PC_DH_LEN];
	if (pc_x25519(dh, secret, pub) != 0) return -EBADMSG;
	mix_key(hs, dh);
	pc_wipe(dh, sizeof dh);
	return 0;
}

static void dead(struct noise_hs *hs) { hs->step = -1; }

int noise_write(struct noise_hs *hs, const uint8_t *payload, size_t plen, uint8_t *out, size_t cap)
{
	int r;
	size_t off = 0;
	if (plen > NOISE_MAX_PAYLOAD) return -EMSGSIZE;
	int my_turn = hs->role == NOISE_INITIATOR ? (hs->step == 0 || hs->step == 2) : hs->step == 1;
	if (hs->step < 0 || hs->step > 2 || !my_turn) return -EPROTO;
	if (cap < NOISE_MAX_MSG) return -ENOSPC;

	if (hs->step == 0 || hs->step == 1) { /* "e" */
		if ((r = make_ephemeral(hs)) != 0) { dead(hs); return r; }
		memcpy(out, hs->e_pub, PC_DH_LEN);
		off = PC_DH_LEN;
		mix_hash(hs, hs->e_pub, PC_DH_LEN);
	}
	if (hs->step == 1) { /* "ee, s, es" */
		if ((r = dh_mix(hs, hs->e, hs->re)) != 0) { dead(hs); return r; }
		off += encrypt_and_hash(hs, hs->s_pub, PC_DH_LEN, out + off);
		if ((r = dh_mix(hs, hs->s, hs->re)) != 0) { dead(hs); return r; }
	}
	if (hs->step == 2) { /* "s, se" */
		off += encrypt_and_hash(hs, hs->s_pub, PC_DH_LEN, out + off);
		if ((r = dh_mix(hs, hs->s, hs->re)) != 0) { dead(hs); return r; }
	}
	if (plen) memcpy(out + off, payload, plen);
	off += encrypt_and_hash(hs, out + off, plen, out + off);
	hs->step++;
	return (int)off;
}

int noise_read(struct noise_hs *hs, const uint8_t *msg, size_t len, uint8_t *payload, size_t cap)
{
	int r;
	size_t off = 0;
	int their_turn = hs->role == NOISE_RESPONDER ? (hs->step == 0 || hs->step == 2) : hs->step == 1;
	if (hs->step < 0 || hs->step > 2 || !their_turn) return -EPROTO;
	if (len > NOISE_MAX_MSG) { dead(hs); return -EINVAL; }

	if (hs->step == 0 || hs->step == 1) { /* "e" */
		if (len < PC_DH_LEN) { dead(hs); return -EINVAL; }
		memcpy(hs->re, msg, PC_DH_LEN);
		off = PC_DH_LEN;
		mix_hash(hs, hs->re, PC_DH_LEN);
	}
	if (hs->step == 1) { /* "ee, s, es" */
		if (len < off + PC_DH_LEN + PC_TAG_LEN) { dead(hs); return -EINVAL; }
		if ((r = dh_mix(hs, hs->e, hs->re)) != 0) { dead(hs); return r; }
		if ((r = decrypt_and_hash(hs, msg + off, PC_DH_LEN + PC_TAG_LEN, hs->rs)) < 0) { dead(hs); return r; }
		off += PC_DH_LEN + PC_TAG_LEN;
		if ((r = dh_mix(hs, hs->e, hs->rs)) != 0) { dead(hs); return r; }
	}
	if (hs->step == 2) { /* "s, se" */
		if (len < PC_DH_LEN + PC_TAG_LEN) { dead(hs); return -EINVAL; }
		if ((r = decrypt_and_hash(hs, msg, PC_DH_LEN + PC_TAG_LEN, hs->rs)) < 0) { dead(hs); return r; }
		off = PC_DH_LEN + PC_TAG_LEN;
		if ((r = dh_mix(hs, hs->e, hs->rs)) != 0) { dead(hs); return r; }
	}
	uint8_t plain[NOISE_MAX_MSG];
	r = decrypt_and_hash(hs, msg + off, len - off, plain);
	if (r < 0) { dead(hs); return r; }
	if ((size_t)r > cap) { dead(hs); return -ENOSPC; }
	if (r) memcpy(payload, plain, (size_t)r);
	hs->step++;
	return r;
}

int noise_done(const struct noise_hs *hs) { return hs->step == 3; }

int noise_finish(struct noise_hs *hs, struct noise_result *out)
{
	if (hs->step != 3) return -EPROTO;
	uint8_t t1[PC_HASH_LEN], t2[PC_HASH_LEN], u1[PC_HASH_LEN], u2[PC_HASH_LEN];
	static const uint8_t udp_label[] = "pdisp/udp/v1";
	pc_hkdf(hs->ck, NULL, 0, t1, t2, NULL);                       /* Noise Split(): the two transport ciphers */
	pc_hkdf(hs->ck, udp_label, sizeof udp_label - 1, u1, u2, NULL); /* distinct input: independent keys for the datagrams */
	int init = hs->role == NOISE_INITIATOR;
	memcpy(out->ctl_send, init ? t1 : t2, PC_KEY_LEN);
	memcpy(out->ctl_recv, init ? t2 : t1, PC_KEY_LEN);
	memcpy(out->udp_send, init ? u1 : u2, PC_KEY_LEN);
	memcpy(out->udp_recv, init ? u2 : u1, PC_KEY_LEN);
	memcpy(out->hash, hs->h, PC_HASH_LEN);
	memcpy(out->peer_static, hs->rs, PC_DH_LEN);
	pc_wipe(t1, sizeof t1); pc_wipe(t2, sizeof t2); pc_wipe(u1, sizeof u1); pc_wipe(u2, sizeof u2);
	pc_wipe(hs, sizeof *hs);
	hs->step = -1;
	return 0;
}

uint32_t noise_sas(const uint8_t hash[PC_HASH_LEN])
{
	static const uint8_t label[] = "pdisp/sas/v1";
	uint8_t d[PC_HASH_LEN];
	pc_hash2(d, hash, PC_HASH_LEN, label, sizeof label - 1);
	uint32_t v = (uint32_t)d[0] << 24 | (uint32_t)d[1] << 16 | (uint32_t)d[2] << 8 | d[3];
	return v % 1000000u;
}
