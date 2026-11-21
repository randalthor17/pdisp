/* Runs the Noise XX handshake (noise.h) over the control channel and switches the channel to encryption (docs/security.md).
 * The phone calls hs_initiate(), the PC hs_respond(); both must be called with the channel still plain, before any HELLO. */
#ifndef PDISP_HANDSHAKE_H
#define PDISP_HANDSHAKE_H

#include "identity.h"
#include "net.h"
#include "secure.h"

#define HS_NAME_MAX 64

struct hs_peer {
	uint8_t pub[PC_DH_LEN]; /* the peer's authenticated static public key */
	char name[HS_NAME_MAX]; /* the name it sent in its handshake payload (not authenticated by anything but the key) */
	uint32_t sas;           /* six-digit code, the same on both sides unless someone sits in the middle */
};

/* Both return 0 with the channel secured and `udp` ready for the datagrams, or a negative errno: -EPROTO (the peer did not speak
 * the handshake, e.g. a plain client), -EBADMSG (authentication failed), -ETIMEDOUT, or the transport's own error. `my_name` is
 * sent as the payload. The caller decides, from `peer->pub`, whether it trusts the peer. */
int hs_initiate(struct net_ctl *c, const struct identity *id, const char *my_name, int timeout_ms, struct hs_peer *peer, struct sec_udp *udp);
int hs_respond(struct net_ctl *c, const struct identity *id, const char *my_name, int timeout_ms, struct hs_peer *peer, struct sec_udp *udp);

#endif
