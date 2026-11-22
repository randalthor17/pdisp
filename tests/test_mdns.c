/* mDNS responder: the packets it builds decode as standard DNS, and only queries meant for it match. */
#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../daemon/mdns.h"

static unsigned u16(const uint8_t *p) { return (p[0] << 8) | p[1]; }

/* skips a name (labels, or a pointer); returns the offset after it */
static size_t skip_name(const uint8_t *p, size_t off)
{
	for (;;) {
		if ((p[off] & 0xc0) == 0xc0) return off + 2;
		if (!p[off]) return off + 1;
		off += 1 + p[off];
	}
}

static size_t query(uint8_t *b, const char *labels_dotted, unsigned type)
{
	size_t n = 0;
	memset(b, 0, 12); b[5] = 1; n = 12;
	const char *s = labels_dotted;
	while (*s) {
		const char *d = strchr(s, '.');
		size_t l = d ? (size_t)(d - s) : strlen(s);
		b[n++] = (uint8_t)l; memcpy(b + n, s, l); n += l; s += l; if (*s == '.') s++;
	}
	b[n++] = 0; b[n++] = type >> 8; b[n++] = type & 255; b[n++] = 0; b[n++] = 1;
	return n;
}

int main(void)
{
	const char *txt[] = { "v=1", "name=desk", NULL };
	struct mdns_svc s;
	mdns_svc_init(&s, "pdisp on desk", "pdisp-desk", 47110, txt);

	uint8_t r[900];
	uint32_t ip = inet_addr("192.168.1.20");
	size_t n = mdns_build_response(r, sizeof r, &s, ip, 120);
	assert(n > 60);
	assert(u16(r + 2) == 0x8400 && u16(r + 4) == 0 && u16(r + 6) == 1 && u16(r + 10) == 3);

	/* walk the 4 records */
	size_t off = 12;
	unsigned seen = 0;
	for (int i = 0; i < 4; i++) {
		off = skip_name(r, off);
		unsigned type = u16(r + off), rdlen = u16(r + off + 8);
		const uint8_t *rd = r + off + 10;
		assert(((r[off + 4] << 24) | (r[off + 5] << 16) | (r[off + 6] << 8) | r[off + 7]) == 120);
		if (type == 12) { seen |= 1; assert(rd[0] == strlen("pdisp on desk") && !memcmp(rd + 1, "pdisp on desk", rd[0])); }
		if (type == 33) { seen |= 2; assert(u16(rd + 4) == 47110); }
		if (type == 16) { seen |= 4; assert(rd[0] == 3 && !memcmp(rd + 1, "v=1", 3) && rd[4] == 9 && !memcmp(rd + 5, "name=desk", 9)); }
		if (type == 1) { seen |= 8; assert(rdlen == 4 && !memcmp(rd, &ip, 4)); }
		off += 10 + rdlen;
	}
	assert(seen == 15 && off == n);

	/* the goodbye has TTL 0 */
	n = mdns_build_response(r, sizeof r, &s, ip, 0);
	off = skip_name(r, 12);
	assert(r[off + 4] == 0 && r[off + 5] == 0 && r[off + 6] == 0 && r[off + 7] == 0);
	/* too small a buffer is refused */
	assert(mdns_build_response(r, 40, &s, ip, 120) == 0);

	uint8_t q[300];
	n = query(q, "_pdisp._tcp.local", 12); assert(mdns_query_matches(q, n, &s));
	n = query(q, "_PDISP._tcp.local", 12); assert(mdns_query_matches(q, n, &s));  /* names are case-insensitive */
	n = query(q, "_pdisp._tcp.local", 255); assert(mdns_query_matches(q, n, &s));
	n = query(q, "pdisp-desk.local", 1); assert(mdns_query_matches(q, n, &s));
	n = query(q, "_http._tcp.local", 12); assert(!mdns_query_matches(q, n, &s));
	n = query(q, "_pdisp._tcp.local", 1); assert(!mdns_query_matches(q, n, &s));   /* wrong type */
	n = query(q, "_pdisp._tcp.local", 12); q[2] = 0x84; assert(!mdns_query_matches(q, n, &s)); /* a response is not a query */
	assert(!mdns_query_matches(q, 5, &s));                                          /* truncated */
	puts("mdns ok");
	return 0;
}
