#include "usbmux.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <time.h>
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
	atomic_int rx_done;   /* the read thread has finished with the link */
	atomic_int tx_done;   /* the write thread has finished with the link */
	int wake[2];          /* pipe that wakes the threads on stop */
	pthread_t tx, rx;
	int started;
	unsigned flags;
	atomic_int refs; /* the owner, and the read and write threads while they run */
};

static void unref(struct usbmux *m)
{
	if (atomic_fetch_sub(&m->refs, 1) != 1) return;
	close(m->ctl_in);
	close(m->dgram_in);
	close(m->wake[0]);
	close(m->wake[1]);
	free(m);
}

static int write_all(struct usbmux *m, int fd, const uint8_t *p, size_t n)
{
	while (n) {
		ssize_t w = write(fd, p, n);
		if (w < 0) {
			if (errno == EINTR) { if (atomic_load(&m->stop)) return -EINTR; continue; } /* a write blocked on a host that does not read is ended by stop */ 
			return -errno;
		}
		p += w;
		n -= (size_t)w;
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
				if (write_all(m, m->link_wr, buf, 5 + (size_t)n) != 0) break;
			}
		}
		if (pf[0].revents & (POLLIN | POLLHUP)) {
			ssize_t n = recv(m->ctl_in, buf + 5, 16384, MSG_DONTWAIT);
			if (n == 0) break; /* the application closed its control connection */
			if (n > 0) {
				put_u32(buf, (uint32_t)n + 1);
				buf[4] = USBMUX_KIND_CTL;
				if (write_all(m, m->link_wr, buf, 5 + (size_t)n) != 0) break;
			}
		}
	}
	mark_down(m);
	atomic_store(&m->tx_done, 1);
	unref(m);
	return NULL;
}

/* link -> application. Android's accessory device returns one USB transfer per read() and throws away what does not fit the
 * caller's buffer, so the link is always read in large chunks and the frames are reassembled here. */
#define RX_CHUNK 16384

static void *rx_thread(void *arg)
{
	struct usbmux *m = arg;
	size_t cap = USBMUX_MAX_FRAME + 5 + RX_CHUNK, have = 0, pos = 0;
	uint8_t *buf = malloc(cap);
	while (buf && !atomic_load(&m->stop)) {
		/* the frames in buf[pos, have) */
		while (have - pos >= 5) {
			uint32_t len = get_u32(buf + pos);
			if (len < 1 || len - 1 > USBMUX_MAX_FRAME) goto done; /* a broken link: nothing to resynchronise to */
			if (have - pos < 4 + (size_t)len) break;
			uint8_t kind = buf[pos + 4];
			const uint8_t *body = buf + pos + 5;
			if (kind == USBMUX_KIND_CTL) {
				if (write_all(m, m->ctl_in, body, len - 1) != 0) goto done;
			} else if (kind == USBMUX_KIND_DGRAM) {
				/* the pipe is reliable, so a full socket means "wait", not "drop": a keyframe is a burst far bigger than a socket
				 * buffer. Only a reader that stops for 100 ms loses datagrams. */
				for (;;) {
					if (send(m->dgram_in, body, len - 1, MSG_DONTWAIT) >= 0) break;
					if (errno == EINTR) continue;
					if (errno != EAGAIN && errno != ENOBUFS) goto done;
					struct pollfd wf = { .fd = m->dgram_in, .events = POLLOUT };
					if (poll(&wf, 1, 100) <= 0 || atomic_load(&m->stop)) break;
				}
			} /* unknown kinds are skipped */
			pos += 4 + (size_t)len;
		}
		if (pos) { memmove(buf, buf + pos, have - pos); have -= pos; pos = 0; }
		struct pollfd pf[2] = { { .fd = m->link_rd, .events = POLLIN }, { .fd = m->wake[0], .events = POLLIN } };
		if (poll(pf, 2, -1) < 0 && errno != EINTR) break;
		if (pf[1].revents) break;
		if (!(pf[0].revents & (POLLIN | POLLHUP | POLLERR))) continue;
		ssize_t r = read(m->link_rd, buf + have, RX_CHUNK);
		if (r == 0) break;
		if (r < 0) {
			if (errno == EINTR || errno == EAGAIN) continue;
			break;
		}
		have += (size_t)r;
	}
done:
	free(buf);
	mark_down(m);
	atomic_store(&m->rx_done, 1);
	unref(m);
	return NULL;
}

static void interrupt_handler(int sig) { (void)sig; } /* only has to make a blocked read return EINTR */

int usbmux_start(int link_rd, int link_wr, unsigned flags, struct usbmux **out, int *ctl, int *dgram)
{
	if (flags & USBMUX_LINK_BLOCKING_READ) {
		struct sigaction sa;
		memset(&sa, 0, sizeof sa);
		sa.sa_handler = interrupt_handler; /* no SA_RESTART: the read must return */
		sigemptyset(&sa.sa_mask);
		sigaction(SIGUSR2, &sa, NULL);
	}
	struct usbmux *m = calloc(1, sizeof *m);
	if (!m) return -ENOMEM;
	m->link_rd = link_rd;
	m->link_wr = link_wr;
	m->flags = flags;
	atomic_store(&m->refs, 3);
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
	if (pthread_create(&m->tx, NULL, tx_thread, m) != 0) {
		close(m->ctl_in); close(m->dgram_in); close(m->wake[0]); close(m->wake[1]); close(cs[1]); close(ds[1]);
		free(m);
		return -EAGAIN;
	}
	if (pthread_create(&m->rx, NULL, rx_thread, m) != 0) {
		atomic_store(&m->refs, 2); /* the owner and the write thread; the read thread never started */
		atomic_store(&m->stop, 1);
		(void)!write(m->wake[1], "x", 1);
		pthread_join(m->tx, NULL);
		close(cs[1]); close(ds[1]);
		unref(m);
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
	if (m->flags & USBMUX_LINK_BLOCKING_READ) { /* a write to an accessory the host does not read can block as well: a signal ends it */
		for (int i = 0; i < 100 && !atomic_load(&m->tx_done); i++) {
			pthread_kill(m->tx, SIGUSR2);
			struct timespec ts = { 0, 5 * 1000 * 1000 };
			nanosleep(&ts, NULL);
		}
		if (atomic_load(&m->tx_done)) pthread_join(m->tx, NULL);
		else pthread_detach(m->tx);
	} else pthread_join(m->tx, NULL);
	if (m->flags & USBMUX_LINK_BLOCKING_READ) {
		/* A read that cannot be polled stays inside the driver, and for as long as it does the process keeps the device open, which on
		 * Android makes the system refuse the next accessory (it cannot open /dev/usb_accessory). A signal ends the read with EINTR. */
		for (int i = 0; i < 100 && !atomic_load(&m->rx_done); i++) {
			pthread_kill(m->rx, SIGUSR2);
			struct timespec ts = { 0, 5 * 1000 * 1000 };
			nanosleep(&ts, NULL);
		}
		if (atomic_load(&m->rx_done)) pthread_join(m->rx, NULL);
		else pthread_detach(m->rx); /* it ends when the link does, and frees the mux then */
	} else pthread_join(m->rx, NULL);
	unref(m);
}
