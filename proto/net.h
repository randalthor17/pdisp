/* Small POSIX networking helpers shared by the daemon and the test client: monotonic time, TCP and UDP sockets, and a
 * blocking-with-timeout reader/writer for the framed control channel (docs/protocol.md section 2). */
#ifndef PDISP_NET_H
#define PDISP_NET_H

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

uint64_t net_now_us(void); /* CLOCK_MONOTONIC in microseconds */

/* Returns a listening TCP socket on `port` (all interfaces), or a negative errno. */
int net_tcp_listen(unsigned port);
/* Connects to host:port, or returns a negative errno. */
int net_tcp_connect(const char *host, unsigned port);
/* A UDP socket bound to `port` (0 = any), non-blocking. */
int net_udp_bind(unsigned port);
/* Sets TCP_NODELAY, or the low-latency TOS (DSCP AF41-ish) on a UDP socket. Failures are ignored. */
void net_tune_tcp(int fd);
void net_tune_udp(int fd);

/* Framed control channel over a stream socket. */
struct net_ctl {
	int fd;
	uint8_t *buf;
	size_t len, cap, consumed;
};

void net_ctl_init(struct net_ctl *c, int fd);
void net_ctl_close(struct net_ctl *c);
/* Sends one message. Returns 0, or a negative errno. */
int net_ctl_send(struct net_ctl *c, uint16_t type, const uint8_t *payload, size_t len);
/* Waits up to `timeout_ms` (-1 = forever) for one message. Returns 1 with the message (valid until the next call), 0 on
 * timeout, -ECONNRESET if the peer closed, or another negative errno (-EINVAL for a malformed frame). */
int net_ctl_recv(struct net_ctl *c, uint16_t *type, const uint8_t **payload, size_t *len, int timeout_ms);

#endif
