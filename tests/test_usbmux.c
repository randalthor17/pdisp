/* usbmux: two muxes joined by pipes behave like a TCP connection plus a datagram socket. */
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "usbmux.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

struct pair { struct usbmux *a, *b; int actl, adg, bctl, bdg; int ab[2], ba[2]; };

static int setup(struct pair *p)
{
	if (pipe(p->ab) || pipe(p->ba)) return -1;
	if (usbmux_start(p->ba[0], p->ab[1], 0, &p->a, &p->actl, &p->adg)) return -1; /* A reads ba, writes ab */
	if (usbmux_start(p->ab[0], p->ba[1], 0, &p->b, &p->bctl, &p->bdg)) return -1;
	return 0;
}

static int read_n(int fd, uint8_t *p, size_t n)
{
	while (n) {
		struct pollfd pf = { .fd = fd, .events = POLLIN };
		if (poll(&pf, 1, 5000) <= 0) return -1;
		ssize_t r = read(fd, p, n);
		if (r <= 0) return -1;
		p += r; n -= (size_t)r;
	}
	return 0;
}

struct writer { int fd; const uint8_t *data; size_t n; };
static void *write_thread(void *a)
{
	struct writer *w = a;
	size_t off = 0;
	while (off < w->n) {
		ssize_t r = write(w->fd, w->data + off, w->n - off);
		if (r <= 0) break;
		off += (size_t)r;
	}
	return NULL;
}

int main(void)
{
	struct pair p;
	CHECK(setup(&p) == 0);

	/* control stream: 600 KB in both directions at once, byte exact (more than any pipe or socket buffer holds) */
	size_t n = 600000;
	uint8_t *src = malloc(n), *got = malloc(n), *src2 = malloc(n), *got2 = malloc(n);
	for (size_t i = 0; i < n; i++) { src[i] = (uint8_t)(i * 7 + (i >> 8)); src2[i] = (uint8_t)(i * 13 + 5); }
	struct writer w1 = { p.actl, src, n }, w2 = { p.bctl, src2, n };
	pthread_t t1, t2;
	pthread_create(&t1, NULL, write_thread, &w1);
	pthread_create(&t2, NULL, write_thread, &w2);
	CHECK(read_n(p.bctl, got, n) == 0 && memcmp(got, src, n) == 0);
	CHECK(read_n(p.actl, got2, n) == 0 && memcmp(got2, src2, n) == 0);
	pthread_join(t1, NULL);
	pthread_join(t2, NULL);

	/* datagrams: boundaries and order preserved, empty and large ones included */
	uint8_t d[65000];
	for (int i = 0; i < 500; i++) {
		size_t len = (size_t)(i * 131) % 60000;
		memset(d, i, len);
		CHECK(send(i & 1 ? p.bdg : p.adg, d, len, 0) == (ssize_t)len);
		uint8_t r[65000];
		struct pollfd pf = { .fd = i & 1 ? p.adg : p.bdg, .events = POLLIN };
		CHECK(poll(&pf, 1, 3000) == 1);
		ssize_t g = recv(pf.fd, r, sizeof r, 0);
		CHECK(g == (ssize_t)len);
		CHECK(len == 0 || (r[0] == (uint8_t)i && r[len - 1] == (uint8_t)i));
	}

	/* a burst: 3000 small datagrams sent without reading, then read in order */
	for (int i = 0; i < 3000; i++) { uint8_t x[4] = { (uint8_t)i, (uint8_t)(i >> 8), 1, 2 }; CHECK(send(p.adg, x, 4, 0) == 4); }
	int in_order = 0;
	for (int i = 0; i < 3000; i++) {
		uint8_t x[8];
		struct pollfd pf = { .fd = p.bdg, .events = POLLIN };
		if (poll(&pf, 1, 3000) != 1) break;
		if (recv(p.bdg, x, sizeof x, 0) != 4) break;
		if (x[0] == (uint8_t)i && x[1] == (uint8_t)(i >> 8)) in_order++;
	}
	CHECK(in_order == 3000);

	/* the link dies: both sides see their control connection close */
	close(p.ab[1]); /* the only writer into B's reader */
	uint8_t one;
	struct pollfd pf = { .fd = p.bctl, .events = POLLIN };
	CHECK(poll(&pf, 1, 3000) == 1 && read(p.bctl, &one, 1) <= 0);
	CHECK(usbmux_link_down(p.b));
	usbmux_stop(p.b);
	close(p.ba[1]);
	usbmux_stop(p.a);

	free(src); free(got); free(src2); free(got2);
	if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
	puts("usbmux: all checks passed");
	return 0;
}
