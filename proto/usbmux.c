#include "usbmux.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

struct usbmux {
	int link_rd, link_wr;
	int ctl_in, dgram_in; /* the mux's ends of the two local sockets */
	atomic_int stop;
	atomic_int down;
	int wake[2];          /* pipe that wakes the threads on stop */
	pthread_t tx, rx;
	int started;
};

static int write_all(int fd, const uint8_t *p, size_t n)
{
	while (n) {
		ssize_t w = write(fd, p, n);
		if (w < 0) {
			if (errno == EINTR) continue;
			return -errno;
		}
		p += w;
		n -= (size_t)w;
	}
	return 0;
}

static int read_all(struct usbmux *m, int fd, uint8_t *p, size_t n)
{
	while (n) {
		struct pollfd pf[2] = { { .fd = fd, .events = POLLIN }, { .fd = m->wake[0], .events = POLLIN } };
		if (poll(pf, 2, -1) < 0 && errno != EINTR) return -errno;
		if (pf[1].revents) return -ECANCELED;
		if (!(pf[0].revents & (POLLIN | POLLHUP | POLLERR))) continue;
		ssize_t r = read(fd, p, n);
		if (r == 0) return -ECONNRESET;
		if (r < 0) {
			if (errno == EINTR || errno == EAGAIN) continue;
			return -errno;
		}
		p += r;
		n -= (size_t)r;
	}
	return 0;
}

static void put_u32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint32_t get_u32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static void mark_down(struct usbmux *m)
{
	atomic_store(&m->down, 1);
	atomic_store(&m->stop, 1);
	(void)!write(m->wake[1], "x", 1);
	shutdown(m->ctl_in, SHUT_RDWR); /* the application sees its control connection close */
}

/* application -> link */
static void *tx_thread(void *arg)
{
	struct usbmux *m = arg;
	uint8_t buf[5 + 65536];
	while (!atomic_load(&m->stop)) {
		struct pollfd pf[3] = { { .fd = m->ctl_in, .events = POLLIN }, { .fd = m->dgram_in, .events = POLLIN },
					{ .fd = m->wake[0], .events = POLLIN } };
		if (poll(pf, 3, -1) < 0 && errno != EINTR) break;
		if (pf[2].revents) break;
		if (pf[1].revents & POLLIN) { /* datagrams first: they are the latency-critical ones */
			ssize_t n = recv(m->dgram_in, buf + 5, 65535, MSG_DONTWAIT);
			if (n >= 0) {
				put_u32(buf, (uint32_t)n + 1);
				buf[4] = USBMUX_KIND_DGRAM;
				if (write_all(m->link_wr, buf, 5 + (size_t)n) != 0) break;
			}
		}
		if (pf[0].revents & (POLLIN | POLLHUP)) {
			ssize_t n = recv(m->ctl_in, buf + 5, 16384, MSG_DONTWAIT);
			if (n == 0) break; /* the application closed its control connection */
			if (n > 0) {
				put_u32(buf, (uint32_t)n + 1);
				buf[4] = USBMUX_KIND_CTL;
				if (write_all(m->link_wr, buf, 5 + (size_t)n) != 0) break;
			}
		}
	}
	mark_down(m);
	return NULL;
}

/* link -> application */
static void *rx_thread(void *arg)
{
	struct usbmux *m = arg;
	uint8_t *buf = malloc(USBMUX_MAX_FRAME);
	while (buf && !atomic_load(&m->stop)) {
		uint8_t h[5];
		if (read_all(m, m->link_rd, h, 5) != 0) break;
		uint32_t len = get_u32(h);
		if (len < 1 || len - 1 > USBMUX_MAX_FRAME) break; /* a broken link: nothing to resynchronise to */
		if (len > 1 && read_all(m, m->link_rd, buf, len - 1) != 0) break;
		if (h[4] == USBMUX_KIND_CTL) {
			if (write_all(m->ctl_in, buf, len - 1) != 0) break;
		} else if (h[4] == USBMUX_KIND_DGRAM) {
			if (send(m->dgram_in, buf, len - 1, MSG_DONTWAIT) < 0 && errno != EAGAIN && errno != ENOBUFS) break;
		} /* unknown kinds are skipped */
	}
	free(buf);
	mark_down(m);
	return NULL;
}

int usbmux_start(int link_rd, int link_wr, struct usbmux **out, int *ctl, int *dgram)
{
	struct usbmux *m = calloc(1, sizeof *m);
	if (!m) return -ENOMEM;
	m->link_rd = link_rd;
	m->link_wr = link_wr;
	int cs[2], ds[2];
	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, cs) != 0) { free(m); return -errno; }
	if (socketpair(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0, ds) != 0) { close(cs[0]); close(cs[1]); free(m); return -errno; }
	if (pipe(m->wake) != 0) { close(cs[0]); close(cs[1]); close(ds[0]); close(ds[1]); free(m); return -errno; }
	int big = 4 << 20; /* a keyframe is a burst of packets */
	setsockopt(ds[0], SOL_SOCKET, SO_SNDBUF, &big, sizeof big);
	setsockopt(ds[1], SOL_SOCKET, SO_RCVBUF, &big, sizeof big);
	setsockopt(ds[1], SOL_SOCKET, SO_SNDBUF, &big, sizeof big);
	setsockopt(ds[0], SOL_SOCKET, SO_RCVBUF, &big, sizeof big);
	m->ctl_in = cs[0];
	m->dgram_in = ds[0];
	if (pthread_create(&m->tx, NULL, tx_thread, m) != 0 || pthread_create(&m->rx, NULL, rx_thread, m) != 0) {
		usbmux_stop(m);
		close(cs[1]);
		close(ds[1]);
		return -EAGAIN;
	}
	m->started = 1;
	*out = m;
	*ctl = cs[1];
	*dgram = ds[1];
	return 0;
}

int usbmux_link_down(const struct usbmux *m)
{
	return atomic_load(&m->down);
}

void usbmux_stop(struct usbmux *m)
{
	if (!m) return;
	atomic_store(&m->stop, 1);
	(void)!write(m->wake[1], "x", 1);
	if (m->started) {
		pthread_join(m->tx, NULL);
		pthread_join(m->rx, NULL);
	}
	close(m->ctl_in);
	close(m->dgram_in);
	close(m->wake[0]);
	close(m->wake[1]);
	free(m);
}
