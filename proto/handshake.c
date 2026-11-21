#include "handshake.h"

#include <errno.h>
#include <string.h>

#include "noise.h"
#include "proto.h"

static const uint8_t prologue[] = "pdisp/1";

static int send_msg(struct net_ctl *c, struct noise_hs *hs, const char *name)
{
	uint8_t msg[NOISE_MAX_MSG];
	size_t n = strlen(name);
	if (n > HS_NAME_MAX - 1) n = HS_NAME_MAX - 1;
	int len = noise_write(hs, (const uint8_t *)name, n, msg, sizeof msg);
	if (len < 0) return len;
	return net_ctl_send(c, PROTO_CTL_HANDSHAKE, msg, (size_t)len);
}

static int recv_msg(struct net_ctl *c, struct noise_hs *hs, int timeout_ms, char name[HS_NAME_MAX])
{
	uint16_t type;
	const uint8_t *payload;
	size_t plen;
	int r = net_ctl_recv(c, &type, &payload, &plen, timeout_ms);
	if (r == 0) return -ETIMEDOUT;
	if (r < 0) return r == -EINVAL ? -EPROTO : r;
	if (type != PROTO_CTL_HANDSHAKE) return -EPROTO; /* a plain client, or garbage */
	uint8_t pl[NOISE_MAX_PAYLOAD];
	int n = noise_read(hs, payload, plen, pl, sizeof pl);
	if (n < 0) return n;
	if (name) {
		size_t k = (size_t)n < HS_NAME_MAX - 1 ? (size_t)n : HS_NAME_MAX - 1;
		memcpy(name, pl, k);
		name[k] = 0;
		for (size_t i = 0; i < k; i++) if ((unsigned char)name[i] < 0x20) name[i] = '?'; /* the name ends up in logs and prompts */
	}
	return 0;
}

static int finish(struct net_ctl *c, struct noise_hs *hs, int initiator, struct hs_peer *peer, struct sec_udp *udp)
{
	struct noise_result res;
	int r = noise_finish(hs, &res);
	if (r != 0) return r;
	memcpy(peer->pub, res.peer_static, PC_DH_LEN);
	peer->sas = noise_sas(res.hash);
	net_ctl_secure(c, res.ctl_send, res.ctl_recv);
	sec_udp_init(udp, res.udp_send, res.udp_recv, initiator);
	pc_wipe(&res, sizeof res);
	return 0;
}

int hs_initiate(struct net_ctl *c, const struct identity *id, const char *my_name, int timeout_ms, struct hs_peer *peer, struct sec_udp *udp)
{
	struct noise_hs hs;
	noise_init(&hs, NOISE_INITIATOR, id->secret, prologue, sizeof prologue - 1);
	memset(peer->name, 0, sizeof peer->name);
	int r = send_msg(c, &hs, "");                       /* -> e */
	if (r == 0) r = recv_msg(c, &hs, timeout_ms, peer->name); /* <- e, ee, s, es (payload: the PC's name) */
	if (r == 0) r = send_msg(c, &hs, my_name);          /* -> s, se */
	if (r == 0) r = finish(c, &hs, 1, peer, udp);
	pc_wipe(&hs, sizeof hs);
	return r;
}

int hs_respond(struct net_ctl *c, const struct identity *id, const char *my_name, int timeout_ms, struct hs_peer *peer, struct sec_udp *udp)
{
	struct noise_hs hs;
	noise_init(&hs, NOISE_RESPONDER, id->secret, prologue, sizeof prologue - 1);
	char ignored[HS_NAME_MAX];
	int r = recv_msg(c, &hs, timeout_ms, ignored);       /* <- e */
	if (r == 0) r = send_msg(c, &hs, my_name);           /* -> e, ee, s, es */
	if (r == 0) r = recv_msg(c, &hs, timeout_ms, peer->name); /* <- s, se (payload: the phone's name) */
	if (r == 0) r = finish(c, &hs, 0, peer, udp);
	pc_wipe(&hs, sizeof hs);
	return r;
}
