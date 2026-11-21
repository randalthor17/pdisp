/* The handshake driver over socketpairs: honest peers, plain peers, silent peers, truncated and corrupted handshakes. */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "handshake.h"
#include "proto.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

struct side { struct net_ctl c; struct identity id; struct hs_peer peer; struct sec_udp udp; int r; const char *name; int timeout; };

static void *respond(void *p) { struct side *s = p; s->r = hs_respond(&s->c, &s->id, s->name, s->timeout, &s->peer, &s->udp); return NULL; }

static void new_id(struct identity *id) { pc_random(id->secret, 32); pc_x25519_public(id->pub, id->secret); }

static void setup(struct side *phone, struct side *pc)
{
	int sv[2];
	CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
	net_ctl_init(&phone->c, sv[0]);
	net_ctl_init(&pc->c, sv[1]);
	new_id(&phone->id); new_id(&pc->id);
	phone->name = "Pixel 8a"; pc->name = "workstation";
	phone->timeout = pc->timeout = 2000;
}

int main(void)
{
	/* honest peers */
	struct side phone, pc;
	setup(&phone, &pc);
	pthread_t t;
	pthread_create(&t, NULL, respond, &pc);
	phone.r = hs_initiate(&phone.c, &phone.id, phone.name, phone.timeout, &phone.peer, &phone.udp);
	pthread_join(t, NULL);
	CHECK(phone.r == 0 && pc.r == 0);
	CHECK(memcmp(phone.peer.pub, pc.id.pub, 32) == 0 && memcmp(pc.peer.pub, phone.id.pub, 32) == 0);
	CHECK(strcmp(phone.peer.name, "workstation") == 0 && strcmp(pc.peer.name, "Pixel 8a") == 0);
	CHECK(phone.peer.sas == pc.peer.sas && phone.peer.sas < 1000000);

	/* the channel is encrypted from here on, both ways, and the datagram keys match */
	uint16_t type; const uint8_t *pl; size_t plen;
	CHECK(net_ctl_send(&phone.c, PROTO_CTL_HELLO, (const uint8_t *)"hello", 5) == 0);
	CHECK(net_ctl_recv(&pc.c, &type, &pl, &plen, 1000) == 1 && type == PROTO_CTL_HELLO && plen == 5);
	CHECK(net_ctl_send(&pc.c, PROTO_CTL_SETUP, (const uint8_t *)"setup", 5) == 0);
	CHECK(net_ctl_recv(&phone.c, &type, &pl, &plen, 1000) == 1 && type == PROTO_CTL_SETUP && plen == 5);
	uint8_t pkt[100] = { 0 };
	struct proto_pkt_hdr h = { .session_id = 7, .type = PROTO_PKT_VIDEO, .seq = 42 };
	proto_pkt_hdr_write(&h, pkt, 16);
	int n = sec_udp_seal(&pc.udp, pkt, 40, sizeof pkt);
	CHECK(sec_udp_open(&phone.udp, pkt, (size_t)n) == 40);
	n = sec_udp_seal(&phone.udp, pkt, 40, sizeof pkt);
	CHECK(sec_udp_open(&pc.udp, pkt, (size_t)n) == 40);
	net_ctl_close(&phone.c); net_ctl_close(&pc.c);

	/* a plain client (sends HELLO at once): the PC refuses it and says nothing useful */
	setup(&phone, &pc);
	pthread_create(&t, NULL, respond, &pc);
	CHECK(net_ctl_send(&phone.c, PROTO_CTL_HELLO, (const uint8_t *)"hello", 5) == 0);
	pthread_join(t, NULL);
	CHECK(pc.r == -EPROTO);
	net_ctl_close(&phone.c); net_ctl_close(&pc.c);

	/* a secure phone talking to a PC that only knows the plain protocol (answers with SETUP/REJECT, not HANDSHAKE) */
	setup(&phone, &pc);
	CHECK(net_ctl_send(&pc.c, PROTO_CTL_REJECT, (const uint8_t *)"\x02\x00\x00\x00", 4) == 0);
	CHECK(hs_initiate(&phone.c, &phone.id, phone.name, 500, &phone.peer, &phone.udp) == -EPROTO);
	net_ctl_close(&phone.c); net_ctl_close(&pc.c);

	/* a peer that connects and says nothing, or stops halfway, cannot hold the PC forever */
	setup(&phone, &pc);
	pc.timeout = 200;
	CHECK(hs_respond(&pc.c, &pc.id, pc.name, pc.timeout, &pc.peer, &pc.udp) == -ETIMEDOUT);
	net_ctl_close(&phone.c); net_ctl_close(&pc.c);
	setup(&phone, &pc);
	pc.timeout = 200;
	pthread_create(&t, NULL, respond, &pc);
	{ /* the phone sends message 1, reads message 2, then goes quiet */
		uint8_t pubkey[32];
		pc_random(pubkey, 32);
		CHECK(net_ctl_send(&phone.c, PROTO_CTL_HANDSHAKE, pubkey, 32) == 0); /* a well-formed message 1 (any 32 bytes are a key) */
	}
	pthread_join(t, NULL);
	CHECK(pc.r == -ETIMEDOUT);
	net_ctl_close(&phone.c); net_ctl_close(&pc.c);

	/* the peer hangs up in the middle */
	setup(&phone, &pc);
	pthread_create(&t, NULL, respond, &pc);
	{
		uint8_t pubkey[32];
		pc_random(pubkey, 32);
		net_ctl_send(&phone.c, PROTO_CTL_HANDSHAKE, pubkey, 32);
		close(phone.c.fd);
		phone.c.fd = -1;
	}
	pthread_join(t, NULL);
	CHECK(pc.r < 0);
	net_ctl_close(&pc.c);

	/* corrupted message 3: the PC does not get a channel */
	setup(&phone, &pc);
	pthread_create(&t, NULL, respond, &pc);
	{
		uint8_t m1[32];
		pc_random(m1, 32);
		net_ctl_send(&phone.c, PROTO_CTL_HANDSHAKE, m1, 32);
		uint16_t ty; const uint8_t *py; size_t pn;
		CHECK(net_ctl_recv(&phone.c, &ty, &py, &pn, 1000) == 1 && ty == PROTO_CTL_HANDSHAKE); /* the PC answered message 1 */
		uint8_t m3[48 + 16 + 8];
		pc_random(m3, sizeof m3);
		net_ctl_send(&phone.c, PROTO_CTL_HANDSHAKE, m3, sizeof m3);
	}
	pthread_join(t, NULL);
	CHECK(pc.r == -EBADMSG);
	net_ctl_close(&phone.c); net_ctl_close(&pc.c);

	if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
	puts("handshake: all checks passed");
	return 0;
}
