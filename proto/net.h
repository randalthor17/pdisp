/* Small POSIX networking helpers shared by the daemon and the test client: monotonic time, TCP and UDP sockets, and a
 * blocking-with-timeout reader/writer for the framed control channel (docs/protocol.md section 2). */
#ifndef PDISP_NET_H
#define PDISP_NET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

#include "secure.h"

uint64_t net_now_us(void); /* CLOCK_MONOTONIC in microseconds */

/* Returns a listening TCP socket on `port` (all interfaces), or a negative errno. */
int net_tcp_listen(unsigned port);
/* Connects to host:port, or returns a negative errno. */
int net_tcp_connect(const char *host, unsigned port);
/* Same, but ends with -ECANCELED when cancelled(user) turns true and -ETIMEDOUT after timeout_ms (checked every 100 ms). */
int net_tcp_connect_cancellable(const char *host, unsigned port, int timeout_ms, bool (*cancelled)(void *), void *user);
/* A UDP socket bound to `port` (0 = any), non-blocking. */
int net_udp_bind(unsigned port);
/* Sets TCP_NODELAY, or the low-latency TOS (DSCP AF41-ish) on a UDP socket. Failures are ignored. */
void net_tune_tcp(int fd);
void net_tune_udp(int fd);

/* Framed control channel over a stream socket. */
struct net_ctl {
	int fd;
	uint8_t *buf;            /* bytes from the socket */
	size_t len, cap, consumed;
	int secure;              /* after net_ctl_secure(): frames are encrypted on the wire */
	struct sec_ctl tx, rx;
	uint8_t *pbuf;           /* decrypted frames waiting to be parsed (secure mode only) */
	size_t plen, pcap, pconsumed;
	int sec_err;             /* a failed secure frame: reported once the frames decrypted before it were delivered */
};

void net_ctl_init(struct net_ctl *c, int fd);
void net_ctl_close(struct net_ctl *c);
/* Switches the channel to authenticated encryption with the keys from the handshake. Both sides do it at the same point (after
 * the last handshake message); bytes already received after it are treated as secure frames. A frame that fails authentication
 * makes net_ctl_recv() return -EBADMSG and the connection must be dropped. */
void net_ctl_secure(struct net_ctl *c, const uint8_t send_key[PC_KEY_LEN], const uint8_t recv_key[PC_KEY_LEN]);
/* Sends one message. Returns 0, or a negative errno. */
int net_ctl_send(struct net_ctl *c, uint16_t type, const uint8_t *payload, size_t len);
/* Waits up to `timeout_ms` (-1 = forever) for one message. Returns 1 with the message (valid until the next call), 0 on
 * timeout, -ECONNRESET if the peer closed, or another negative errno (-EINVAL for a malformed frame). */
int net_ctl_recv(struct net_ctl *c, uint16_t *type, const uint8_t **payload, size_t *len, int timeout_ms);

#endif
