/* pdisp-ctl: talk to a running pdispd over its control socket (docs/control.md).
 *   pdisp-ctl [-s SOCKET] status               one line: the daemon's state as JSON
 *   pdisp-ctl [-s SOCKET] watch [N]            print events as they come (N lines, default: until interrupted)
 *   pdisp-ctl [-s SOCKET] devices              the paired devices
 *   pdisp-ctl [-s SOCKET] remove FINGERPRINT   revoke one (a prefix of at least 4 characters)
 *   pdisp-ctl [-s SOCKET] set KEY VALUE        change a setting (refresh_cap_hz, bitrate_kbps, max_bitrate_kbps, grace_secs, refine_frames)
 *   pdisp-ctl [-s SOCKET] disconnect           end the session and remove the monitor
 *   pdisp-ctl [-s SOCKET] pairing [SECS]       listen for pairing requests and print them (and the answers); SECS default 30
 *   pdisp-ctl [-s SOCKET] answer ID always|once|deny
 * The exit status is 0 if the daemon said ok. */
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static int fd = -1;

static int send_line(const char *s)
{
	char b[512];
	int n = snprintf(b, sizeof b, "%s\n", s);
	return n > 0 && write(fd, b, (size_t)n) == n ? 0 : -1;
}

/* the next line from the socket, waiting up to timeout_ms; returns its length, 0 on timeout, -1 on close */
static int next_line(char *out, size_t cap, int timeout_ms)
{
	static char buf[8192];
	static size_t len;
	for (;;) {
		char *nl = memchr(buf, '\n', len);
		if (nl) {
			size_t n = (size_t)(nl - buf);
			if (n >= cap) n = cap - 1;
			memcpy(out, buf, n);
			out[n] = 0;
			size_t used = (size_t)(nl - buf) + 1;
			memmove(buf, buf + used, len - used);
			len -= used;
			return (int)n + 1;
		}
		struct pollfd p = { .fd = fd, .events = POLLIN };
		int r = poll(&p, 1, timeout_ms);
		if (r == 0) return 0;
		if (r < 0) { if (errno == EINTR) continue; return -1; }
		ssize_t k = read(fd, buf + len, sizeof buf - len);
		if (k <= 0) return -1;
		len += (size_t)k;
	}
}

static int is_reply(const char *l) { return strstr(l, "\"reply\"") != NULL; }
static int is_ok(const char *l) { return strstr(l, "\"ok\":true") != NULL; }

int main(int argc, char **argv)
{
	const char *sock = NULL;
	int a = 1;
	if (a + 1 < argc && !strcmp(argv[a], "-s")) { sock = argv[a + 1]; a += 2; }
	if (a >= argc) { fprintf(stderr, "usage: pdisp-ctl [-s SOCKET] status|watch|devices|remove|set|disconnect|pairing|answer ... (see the top of tools/pdisp-ctl.c)\n"); return 2; }
	char path[104]; /* a socket path is at most 107 characters */
	if (!sock) {
		const char *rt = getenv("XDG_RUNTIME_DIR");
		if (!rt) { fprintf(stderr, "XDG_RUNTIME_DIR is not set: give -s SOCKET\n"); return 2; }
		if (snprintf(path, sizeof path, "%s/pdisp/control.sock", rt) >= (int)sizeof path) { fprintf(stderr, "the socket path is too long: give -s SOCKET\n"); return 2; }
		sock = path;
	}
	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	if (strlen(sock) >= sizeof sa.sun_path) { fprintf(stderr, "the socket path is too long\n"); return 2; }
	strcpy(sa.sun_path, sock);
	if (fd < 0 || connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) { fprintf(stderr, "cannot connect to %s: %s (is pdispd running?)\n", sock, strerror(errno)); return 1; }

	const char *cmd = argv[a];
	char line[2048], req[512];
	if (!strcmp(cmd, "status")) {
		send_line("{\"cmd\":\"get_state\",\"req\":\"1\"}");
		for (int i = 0; i < 20; i++) {
			if (next_line(line, sizeof line, 3000) <= 0) { fprintf(stderr, "no answer\n"); return 1; }
			if (strstr(line, "\"event\":\"state\"")) { puts(line); return 0; }
		}
		return 1;
	}
	if (!strcmp(cmd, "watch") || !strcmp(cmd, "pairing")) {
		int pairing = !strcmp(cmd, "pairing");
		int limit = a + 1 < argc ? atoi(argv[a + 1]) : (pairing ? 30 : 0);
		if (pairing) send_line("{\"cmd\":\"subscribe\",\"pairing\":true}");
		time_t end = pairing ? time(NULL) + limit : 0;
		int count = 0;
		for (;;) {
			int r = next_line(line, sizeof line, 500);
			if (r < 0) return 1;
			if (r > 0 && (!pairing || strstr(line, "pairing"))) { puts(line); fflush(stdout); count++; }
			if (!pairing && limit && count >= limit) return 0;
			if (pairing && time(NULL) >= end) return 0;
		}
	}
	if (!strcmp(cmd, "devices")) snprintf(req, sizeof req, "{\"cmd\":\"devices\",\"req\":\"1\"}");
	else if (!strcmp(cmd, "disconnect")) snprintf(req, sizeof req, "{\"cmd\":\"disconnect\",\"req\":\"1\"}");
	else if (!strcmp(cmd, "remove") && a + 1 < argc) snprintf(req, sizeof req, "{\"cmd\":\"remove_device\",\"fingerprint\":\"%s\",\"req\":\"1\"}", argv[a + 1]);
	else if (!strcmp(cmd, "set") && a + 2 < argc) snprintf(req, sizeof req, "{\"cmd\":\"set_config\",\"key\":\"%s\",\"value\":\"%s\",\"req\":\"1\"}", argv[a + 1], argv[a + 2]);
	else if (!strcmp(cmd, "answer") && a + 2 < argc) snprintf(req, sizeof req, "{\"cmd\":\"pair_answer\",\"id\":%s,\"answer\":\"%s\",\"req\":\"1\"}", argv[a + 1], argv[a + 2]);
	else { fprintf(stderr, "bad command line\n"); return 2; }
	send_line(req);
	for (int i = 0; i < 20; i++) {
		int r = next_line(line, sizeof line, 3000);
		if (r <= 0) { fprintf(stderr, "no answer\n"); return 1; }
		if (is_reply(line)) { puts(line); return is_ok(line) ? 0 : 1; }
	}
	return 1;
}
