#define _GNU_SOURCE
#include "mdns.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#define T_A 1
#define T_PTR 12
#define T_TXT 16
#define T_SRV 33
#define T_ANY 255
#define CLASS_IN 1
#define CACHE_FLUSH 0x8000

void mdns_svc_init(struct mdns_svc *s, const char *instance, const char *host, uint16_t port, const char *const *txt)
{
	memset(s, 0, sizeof *s);
	snprintf(s->instance, sizeof s->instance, "%s", instance);
	snprintf(s->host, sizeof s->host, "%s", host);
	s->port = port;
	for (; txt && *txt; txt++) {
		size_t n = strlen(*txt);
		if (n > 100 || s->txt_len + 1 + n > sizeof s->txt) continue;
		s->txt[s->txt_len++] = (uint8_t)n;
		memcpy(s->txt + s->txt_len, *txt, n);
		s->txt_len += n;
	}
	if (!s->txt_len) s->txt[s->txt_len++] = 0; /* an empty TXT record is a single zero byte */
}

/* ---- writing ---- */

struct wr { uint8_t *b; size_t cap, n; bool over; };

static void w8(struct wr *w, unsigned v) { if (w->n + 1 > w->cap) { w->over = true; return; } w->b[w->n++] = (uint8_t)v; }
static void w16(struct wr *w, unsigned v) { w8(w, v >> 8); w8(w, v & 255); }
static void w32(struct wr *w, uint32_t v) { w16(w, v >> 16); w16(w, v & 0xffff); }
static void wbytes(struct wr *w, const void *p, size_t n) { if (w->n + n > w->cap) { w->over = true; return; } memcpy(w->b + w->n, p, n); w->n += n; }

/* A name from labels separated by dots, but the first label (when `first_raw`) is taken whole, dots and spaces included. */
static void wname(struct wr *w, const char *first_raw, const char *rest)
{
	if (first_raw) {
		size_t n = strlen(first_raw);
		if (n > 63) n = 63;
		w8(w, (unsigned)n); wbytes(w, first_raw, n);
	}
	while (*rest) {
		const char *dot = strchr(rest, '.');
		size_t n = dot ? (size_t)(dot - rest) : strlen(rest);
		if (n > 63) n = 63;
		w8(w, (unsigned)n); wbytes(w, rest, n);
		rest += n;
		if (*rest == '.') rest++;
	}
	w8(w, 0);
}

static void host_fqdn(const struct mdns_svc *s, char *out, size_t cap) { snprintf(out, cap, "%s.local", s->host); }

/* the rdata length is written after the data is known */
static size_t rr_begin(struct wr *w, const char *raw_first, const char *rest, unsigned type, bool flush, uint32_t ttl)
{
	wname(w, raw_first, rest);
	w16(w, type);
	w16(w, CLASS_IN | (flush ? CACHE_FLUSH : 0));
	w32(w, ttl);
	w16(w, 0);
	return w->n; /* start of rdata */
}
static void rr_end(struct wr *w, size_t start) { if (!w->over) { size_t len = w->n - start; w->b[start - 2] = (uint8_t)(len >> 8); w->b[start - 1] = (uint8_t)len; } }

size_t mdns_build_response(uint8_t *buf, size_t cap, const struct mdns_svc *s, uint32_t ipv4_be, uint32_t ttl)
{
	struct wr w = { buf, cap, 0, false };
	char host[100];
	host_fqdn(s, host, sizeof host);
	w16(&w, 0); w16(&w, 0x8400);          /* id 0, response + authoritative */
	w16(&w, 0); w16(&w, 1); w16(&w, 0); w16(&w, 3); /* questions, answers (PTR), authority, additional (SRV, TXT, A) */

	size_t r = rr_begin(&w, NULL, MDNS_SERVICE, T_PTR, false, ttl);
	wname(&w, s->instance, MDNS_SERVICE);
	rr_end(&w, r);

	r = rr_begin(&w, s->instance, MDNS_SERVICE, T_SRV, true, ttl);
	w16(&w, 0); w16(&w, 0); w16(&w, s->port);
	wname(&w, NULL, host);
	rr_end(&w, r);

	r = rr_begin(&w, s->instance, MDNS_SERVICE, T_TXT, true, ttl);
	wbytes(&w, s->txt, s->txt_len);
	rr_end(&w, r);

	r = rr_begin(&w, NULL, host, T_A, true, ttl);
	wbytes(&w, &ipv4_be, 4);
	rr_end(&w, r);
	return w.over ? 0 : w.n;
}

/* ---- reading a query ---- */

/* Reads a (possibly compressed) name into `out` as "label.label" (labels joined by dots, no trailing dot). Returns the offset after it, or 0. */
static size_t rname(const uint8_t *p, size_t len, size_t off, char *out, size_t cap)
{
	size_t o = 0, next = 0;
	int jumps = 0;
	out[0] = 0;
	for (;;) {
		if (off >= len) return 0;
		unsigned c = p[off];
		if ((c & 0xc0) == 0xc0) {
			if (off + 1 >= len || ++jumps > 8) return 0;
			if (!next) next = off + 2;
			off = ((c & 0x3f) << 8) | p[off + 1];
			continue;
		}
		if (c & 0xc0) return 0;
		off++;
		if (c == 0) break;
		if (off + c > len || o + c + 2 > cap) return 0;
		if (o) out[o++] = '.';
		memcpy(out + o, p + off, c); o += c; out[o] = 0;
		off += c;
	}
	return next ? next : off;
}

bool mdns_query_matches(const uint8_t *p, size_t len, const struct mdns_svc *s)
{
	if (len < 12 || (p[2] & 0x80)) return false; /* too short, or a response */
	unsigned qd = (p[4] << 8) | p[5];
	char inst[160], host[100], name[260];
	snprintf(inst, sizeof inst, "%s." MDNS_SERVICE, s->instance);
	host_fqdn(s, host, sizeof host);
	size_t off = 12;
	for (unsigned i = 0; i < qd && i < 16; i++) {
		off = rname(p, len, off, name, sizeof name);
		if (!off || off + 4 > len) return false;
		unsigned type = (p[off] << 8) | p[off + 1];
		off += 4;
		if (!strcasecmp(name, MDNS_SERVICE) && (type == T_PTR || type == T_ANY)) return true;
		if (!strcasecmp(name, inst) && (type == T_SRV || type == T_TXT || type == T_ANY)) return true;
		if (!strcasecmp(name, host) && (type == T_A || type == T_ANY)) return true;
	}
	return false;
}

/* ---- the responder ---- */

#define MAX_IF 8
struct ifc { int index; uint32_t addr_be; bool joined; };

struct mdns {
	struct mdns_svc svc;
	int fd;
	int stop_pipe[2];
	pthread_t th;
	struct ifc ifs[MAX_IF];
	int nifs;
};

static void scan_ifaces(struct mdns *m)
{
	struct ifaddrs *all, *a;
	if (getifaddrs(&all)) return;
	for (a = all; a; a = a->ifa_next) {
		if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET) continue;
		if (!(a->ifa_flags & IFF_UP) || !(a->ifa_flags & IFF_MULTICAST) || (a->ifa_flags & IFF_LOOPBACK)) continue;
		int idx = (int)if_nametoindex(a->ifa_name);
		uint32_t addr = ((struct sockaddr_in *)a->ifa_addr)->sin_addr.s_addr;
		struct ifc *f = NULL;
		for (int i = 0; i < m->nifs; i++) if (m->ifs[i].index == idx) { f = &m->ifs[i]; break; }
		if (!f) { if (m->nifs == MAX_IF) continue; f = &m->ifs[m->nifs++]; memset(f, 0, sizeof *f); f->index = idx; }
		f->addr_be = addr;
		if (!f->joined) {
			struct ip_mreqn mr = { .imr_multiaddr.s_addr = inet_addr("224.0.0.251"), .imr_address.s_addr = addr, .imr_ifindex = idx };
			f->joined = setsockopt(m->fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mr, sizeof mr) == 0;
		}
	}
	freeifaddrs(all);
}

static void send_on(struct mdns *m, const struct ifc *f, uint32_t ttl)
{
	uint8_t buf[900];
	size_t n = mdns_build_response(buf, sizeof buf, &m->svc, f->addr_be, ttl);
	if (!n) return;
	struct ip_mreqn mr = { .imr_address.s_addr = f->addr_be, .imr_ifindex = f->index };
	setsockopt(m->fd, IPPROTO_IP, IP_MULTICAST_IF, &mr, sizeof mr);
	struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(5353), .sin_addr.s_addr = inet_addr("224.0.0.251") };
	sendto(m->fd, buf, n, MSG_NOSIGNAL, (struct sockaddr *)&to, sizeof to);
}

static void announce_all(struct mdns *m, uint32_t ttl)
{
	for (int i = 0; i < m->nifs; i++) if (m->ifs[i].joined) send_on(m, &m->ifs[i], ttl);
}

static void *mdns_thread(void *arg)
{
	struct mdns *m = arg;
	uint8_t pkt[1500];
	int ticks = 0;
	scan_ifaces(m);
	announce_all(m, 120);
	for (;;) {
		struct pollfd pf[2] = { { .fd = m->fd, .events = POLLIN }, { .fd = m->stop_pipe[0], .events = POLLIN } };
		int r = poll(pf, 2, 5000);
		if (r > 0 && (pf[1].revents & POLLIN)) break;
		if (r > 0 && (pf[0].revents & POLLIN)) {
			char cbuf[CMSG_SPACE(sizeof(struct in_pktinfo))];
			struct iovec iov = { pkt, sizeof pkt };
			struct sockaddr_in from;
			struct msghdr mh = { .msg_name = &from, .msg_namelen = sizeof from, .msg_iov = &iov, .msg_iovlen = 1, .msg_control = cbuf, .msg_controllen = sizeof cbuf };
			ssize_t n = recvmsg(m->fd, &mh, 0);
			if (n > 0 && mdns_query_matches(pkt, (size_t)n, &m->svc)) {
				int idx = 0;
				for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c))
					if (c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_PKTINFO) idx = ((struct in_pktinfo *)CMSG_DATA(c))->ipi_ifindex;
				bool sent = false; /* a query we looped back to ourselves reports the loopback interface: then answer on all */
				for (int i = 0; i < m->nifs; i++) if (m->ifs[i].joined && m->ifs[i].index == idx) { send_on(m, &m->ifs[i], 120); sent = true; }
				if (!sent) announce_all(m, 120);
			}
		}
		if (r == 0 && ++ticks >= 12) { ticks = 0; announce_all(m, 120); } /* every minute */
		if (r == 0) scan_ifaces(m); /* a network that came up since */
	}
	announce_all(m, 0);
	return NULL;
}

int mdns_start(const struct mdns_svc *s, struct mdns **out)
{
	struct mdns *m = calloc(1, sizeof *m);
	if (!m) return -ENOMEM;
	m->svc = *s;
	m->fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (m->fd < 0) { int e = -errno; free(m); return e; }
	int one = 1, ttl = 255;
	setsockopt(m->fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	setsockopt(m->fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
	setsockopt(m->fd, IPPROTO_IP, IP_PKTINFO, &one, sizeof one);
	setsockopt(m->fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(5353), .sin_addr.s_addr = htonl(INADDR_ANY) };
	if (bind(m->fd, (struct sockaddr *)&a, sizeof a)) { int e = -errno; close(m->fd); free(m); return e; }
	if (pipe2(m->stop_pipe, O_CLOEXEC)) { int e = -errno; close(m->fd); free(m); return e; }
	if (pthread_create(&m->th, NULL, mdns_thread, m)) { close(m->fd); close(m->stop_pipe[0]); close(m->stop_pipe[1]); free(m); return -EAGAIN; }
	*out = m;
	return 0;
}

void mdns_stop(struct mdns *m)
{
	if (!m) return;
	(void)!write(m->stop_pipe[1], "x", 1);
	pthread_join(m->th, NULL);
	close(m->fd); close(m->stop_pipe[0]); close(m->stop_pipe[1]);
	free(m);
}
