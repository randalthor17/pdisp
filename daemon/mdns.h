/* A small mDNS (RFC 6762) responder: announces "_pdisp._tcp" on every IPv4 network the PC is on, so the Android app can list this PC
 * without typing its address (docs/discovery.md). It only answers; it never queries or probes. Standard responders (avahi,
 * systemd-resolved) can run beside it: they share UDP 5353. The packet building and matching are plain functions (tests/test_mdns.c). */
#ifndef PDISP_MDNS_H
#define PDISP_MDNS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MDNS_SERVICE "_pdisp._tcp.local"

struct mdns_svc {
	char instance[64];   /* the name people see, e.g. "pdisp on nixos" */
	char host[64];       /* a host label, e.g. "pdisp-nixos": resolves to the PC's address (".local" is added) */
	uint16_t port;       /* the TCP port the daemon listens on */
	uint8_t txt[256];    /* TXT rdata: length-prefixed "key=value" strings */
	size_t txt_len;
};

/* Fills `s`; `txt` is NULL-terminated "key=value" strings (each under 100 bytes). */
void mdns_svc_init(struct mdns_svc *s, const char *instance, const char *host, uint16_t port, const char *const *txt);

/* Builds the full response (PTR in the answers; SRV, TXT and A as additional records) with the address `ipv4_be` (network order) and `ttl` seconds
 * (0 = "goodbye"). Returns the length, or 0 if `cap` is too small. */
size_t mdns_build_response(uint8_t *buf, size_t cap, const struct mdns_svc *s, uint32_t ipv4_be, uint32_t ttl);

/* True if `pkt` is a query with a question this service answers (the service type, its instance or its host). */
bool mdns_query_matches(const uint8_t *pkt, size_t len, const struct mdns_svc *s);

struct mdns;
/* Starts the responder thread. Returns 0, or a negative errno (-EADDRINUSE etc.): callers carry on without discovery. */
int mdns_start(const struct mdns_svc *s, struct mdns **out);
/* Sends the goodbye (TTL 0) and stops. */
void mdns_stop(struct mdns *m);

#endif
