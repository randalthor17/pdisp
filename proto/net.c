#include "net.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "proto.h"

uint64_t net_now_us(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000 + (uint64_t)t.tv_nsec / 1000;
}

void net_tune_tcp(int fd)
{
	int one = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
}

void net_tune_udp(int fd)
{
	int tos = 0x88; /* AF41: Wi-Fi access points map this to the video access category */
	setsockopt(fd, IPPROTO_IP, IP_TOS, &tos, sizeof tos);
	int buf = 4 << 20;
	setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf, sizeof buf);
	setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf, sizeof buf);
}

int net_tcp_listen(unsigned port)
{
	int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) return -errno;
	int one = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port), .sin_addr.s_addr = htonl(INADDR_ANY) };
	if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0 || listen(fd, 1) < 0) {
		int e = errno;
		close(fd);
		return -e;
	}
	return fd;
}

int net_tcp_connect(const char *host, unsigned port)
{
	char p[16];
	snprintf(p, sizeof p, "%u", port);
	struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *res;
	if (getaddrinfo(host, p, &hints, &res) != 0) return -EHOSTUNREACH;
	int fd = socket(res->ai_family, res->ai_socktype | SOCK_CLOEXEC, 0);
	if (fd < 0 || connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
		int e = errno;
		if (fd >= 0) close(fd);
		freeaddrinfo(res);
		return -e;
	}
	freeaddrinfo(res);
	net_tune_tcp(fd);
	return fd;
}

int net_udp_bind(unsigned port)
{
	int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (fd < 0) return -errno;
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port), .sin_addr.s_addr = htonl(INADDR_ANY) };
	if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0) {
		int e = errno;
		close(fd);
		return -e;
	}
	net_tune_udp(fd);
	return fd;
}

void net_ctl_init(struct net_ctl *c, int fd)
{
	memset(c, 0, sizeof *c);
	c->fd = fd;
}

void net_ctl_close(struct net_ctl *c)
{
	if (c->fd >= 0) close(c->fd);
	free(c->buf);
	memset(c, 0, sizeof *c);
	c->fd = -1;
}

int net_ctl_send(struct net_ctl *c, uint16_t type, const uint8_t *payload, size_t len)
{
	uint8_t *frame = malloc(len + 8);
	if (!frame) return -ENOMEM;
	int n = proto_ctl_frame_write(type, payload, len, frame, len + 8);
	if (n < 0) { free(frame); return n; }
	size_t off = 0;
	while (off < (size_t)n) {
		ssize_t w = send(c->fd, frame + off, (size_t)n - off, MSG_NOSIGNAL);
		if (w < 0) {
			if (errno == EINTR) continue;
			int e = errno;
			free(frame);
			return -e;
		}
		off += (size_t)w;
	}
	free(frame);
	return 0;
}

int net_ctl_recv(struct net_ctl *c, uint16_t *type, const uint8_t **payload, size_t *len, int timeout_ms)
{
	if (c->consumed) { /* drop the message handed out by the previous call */
		memmove(c->buf, c->buf + c->consumed, c->len - c->consumed);
		c->len -= c->consumed;
		c->consumed = 0;
	}
	for (;;) {
		int n = proto_ctl_frame_parse(c->buf, c->len, type, payload, len);
		if (n < 0) return -EINVAL;
		if (n > 0) { c->consumed = (size_t)n; return 1; }

		struct pollfd p = { .fd = c->fd, .events = POLLIN };
		int r = poll(&p, 1, timeout_ms);
		if (r < 0) { if (errno == EINTR) continue; return -errno; }
		if (r == 0) return 0;
		if (c->len + 4096 > c->cap) {
			size_t nc = c->cap ? c->cap * 2 : 8192;
			uint8_t *nb = realloc(c->buf, nc);
			if (!nb) return -ENOMEM;
			c->buf = nb;
			c->cap = nc;
		}
		ssize_t got = recv(c->fd, c->buf + c->len, c->cap - c->len, 0);
		if (got == 0) return -ECONNRESET;
		if (got < 0) { if (errno == EINTR || errno == EAGAIN) continue; return -errno; }
		c->len += (size_t)got;
		if (c->len > PROTO_MAX_CTL_PAYLOAD + 16) return -EMSGSIZE;
	}
}
