#define _GNU_SOURCE
#include "control.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define MAX_CLIENTS 8
#define LINE_MAX_LEN 1024

struct client {
	int fd;
	char buf[LINE_MAX_LEN];
	size_t len;
	bool pairing; /* wants pairing requests */
};

static struct {
	pthread_t thread;
	bool running;
	int lfd;
	int wake[2];
	char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
	struct control_ops ops;
	pthread_mutex_t mu;           /* guards st, st_dirty, the pairing request and nclients_pairing */
	pthread_cond_t cv;
	struct ctl_state st;
	bool st_valid, st_dirty;
	int pairing_clients;          /* clients listening for pairing requests (updated by the thread) */
	/* the pairing request being decided */
	bool pair_pending, pair_answered, pair_announce;
	bool pair_done_pending;
	char pair_done_msg[160];
	unsigned pair_id, pair_code;
	char pair_name[64], pair_fp[40];
	int pair_answer;
	struct client cl[MAX_CLIENTS];
	volatile bool quit;
} G = { .mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER, .lfd = -1 };

static void wake(void) { char c = 1; if (G.wake[1] >= 0) (void)!write(G.wake[1], &c, 1); }

static void send_line(struct client *c, const char *json)
{
	char line[4096];
	int n = snprintf(line, sizeof line, "%s\n", json);
	if (n <= 0 || n >= (int)sizeof line) return;
	/* a slow client must not stall the thread: a full socket buffer drops the message and, if it persists, the client is closed by the reader */
	(void)!send(c->fd, line, (size_t)n, MSG_DONTWAIT | MSG_NOSIGNAL);
}

static void broadcast(const char *json, bool pairing_only)
{
	for (int i = 0; i < MAX_CLIENTS; i++)
		if (G.cl[i].fd >= 0 && (!pairing_only || G.cl[i].pairing)) send_line(&G.cl[i], json);
}

static const char *state_json(char *out, size_t cap, const struct ctl_state *s)
{
	struct jw w;
	jw_init(&w, out, cap);
	jw_obj_begin(&w);
	jw_str(&w, "event", "state");
	jw_bool(&w, "usb", s->usb);
	jw_bool(&w, "net", s->net);
	jw_int(&w, "port", s->port);
	jw_str(&w, "usb_state", s->usb_state);
	if (s->session) {
		jw_obj_member(&w, "session");
		jw_str(&w, "device", s->device);
		jw_str(&w, "transport", s->transport);
		jw_int(&w, "width", s->width);
		jw_int(&w, "height", s->height);
		jw_num(&w, "refresh_hz", s->refresh_hz);
		jw_num(&w, "seconds", s->session_secs);
		jw_num(&w, "fps", s->fps);
		jw_num(&w, "bitrate_mbps", s->bitrate_mbps);
		jw_num(&w, "queue_ms", s->queue_ms);
		jw_num(&w, "phone_queue_ms", s->phone_queue_ms);
		jw_num(&w, "phone_decode_ms", s->phone_decode_ms);
		jw_int(&w, "frames_lost", (long long)s->frames_lost);
		jw_int(&w, "idr_frames", (long long)s->idr_frames);
		jw_int(&w, "repair_packets", (long long)s->repair_packets);
		jw_obj_end(&w);
	} else {
		jw_null(&w, "session");
	}
	if (s->monitor) {
		jw_obj_member(&w, "monitor");
		jw_str(&w, "connector", s->connector);
		jw_bool(&w, "grace", s->grace);
		jw_obj_end(&w);
	} else {
		jw_null(&w, "monitor");
	}
	jw_obj_member(&w, "config");
	jw_int(&w, "refresh_cap_hz", s->refresh_cap_hz);
	jw_int(&w, "bitrate_kbps", s->bitrate_kbps);
	jw_int(&w, "max_bitrate_kbps", s->max_bitrate_kbps);
	jw_int(&w, "grace_secs", s->grace_secs);
	jw_int(&w, "refine_frames", s->refine_frames);
	jw_obj_end(&w);
	jw_obj_end(&w);
	return jw_done(&w);
}

static void reply(struct client *c, const struct jobj *req, const char *cmd, bool ok, const char *error, const struct jw *extra)
{
	char b[3072];
	struct jw w;
	jw_init(&w, b, sizeof b);
	jw_obj_begin(&w);
	jw_str(&w, "reply", cmd);
	jw_bool(&w, "ok", ok);
	const char *rq = json_get(req, "req");
	if (rq) jw_str(&w, "req", rq);
	if (error) jw_str(&w, "error", error);
	/* `extra` is a finished object like {"devices":[...]}: splice its members in */
	if (extra && extra->len > 2) {
		size_t n = extra->len - 2; /* without the braces */
		if (w.len + n + 3 < w.cap) { w.buf[w.len++] = ','; memcpy(w.buf + w.len, extra->buf + 1, n); w.len += n; w.buf[w.len] = 0; }
	}
	jw_obj_end(&w);
	const char *s = jw_done(&w);
	if (s) send_line(c, s);
}

static void send_state(struct client *c)
{
	char b[2048];
	pthread_mutex_lock(&G.mu);
	struct ctl_state s = G.st;
	bool valid = G.st_valid;
	pthread_mutex_unlock(&G.mu);
	if (!valid) return;
	const char *j = state_json(b, sizeof b, &s);
	if (j) send_line(c, j);
}

static void handle_line(struct client *c, const char *line)
{
	struct jobj req;
	if (json_parse_flat(line, &req) != 0) {
		struct jobj none = { .n = 0 };
		reply(c, &none, "error", false, "not a JSON object of plain values", NULL);
		return;
	}
	const char *cmd = json_get(&req, "cmd");
	if (!cmd) { reply(c, &req, "error", false, "no cmd", NULL); return; }

	if (!strcmp(cmd, "get_state")) {
		reply(c, &req, cmd, true, NULL, NULL);
		send_state(c);
	} else if (!strcmp(cmd, "subscribe")) { /* every client gets state events; pairing requests only if it asks */
		bool want = json_get_bool(&req, "pairing", false);
		if (want != c->pairing) {
			c->pairing = want;
			pthread_mutex_lock(&G.mu);
			int n = 0;
			for (int i = 0; i < MAX_CLIENTS; i++) if (G.cl[i].fd >= 0 && G.cl[i].pairing) n++;
			G.pairing_clients = n;
			bool announce = G.pair_pending && !G.pair_answered && want;
			pthread_mutex_unlock(&G.mu);
			if (announce) G.pair_announce = true; /* a request is open: tell the newcomer */
		}
		reply(c, &req, cmd, true, NULL, NULL);
	} else if (!strcmp(cmd, "disconnect")) {
		if (G.ops.disconnect) G.ops.disconnect(G.ops.user);
		reply(c, &req, cmd, true, NULL, NULL);
	} else if (!strcmp(cmd, "pair_answer")) {
		long long id = json_get_int(&req, "id", -1);
		const char *a = json_get(&req, "answer");
		int ans = !a ? 0 : !strcmp(a, "always") ? 'y' : !strcmp(a, "once") ? 'o' : !strcmp(a, "deny") ? 'n' : 0;
		bool ok = false;
		pthread_mutex_lock(&G.mu);
		if (ans && G.pair_pending && !G.pair_answered && (long long)G.pair_id == id) {
			G.pair_answered = true;
			G.pair_answer = ans;
			pthread_cond_broadcast(&G.cv);
			ok = true;
		}
		pthread_mutex_unlock(&G.mu);
		reply(c, &req, cmd, ok, ok ? NULL : "no such pairing request, or it was already answered", NULL);
	} else if (!strcmp(cmd, "devices")) {
		char b[2048];
		struct jw w;
		jw_init(&w, b, sizeof b);
		jw_obj_begin(&w);
		if (G.ops.devices) G.ops.devices(G.ops.user, &w);
		jw_obj_end(&w);
		if (jw_done(&w)) reply(c, &req, cmd, true, NULL, &w);
		else reply(c, &req, cmd, false, "too many devices to list", NULL);
	} else if (!strcmp(cmd, "remove_device")) {
		const char *fp = json_get(&req, "fingerprint");
		int n = (fp && G.ops.remove_device) ? G.ops.remove_device(G.ops.user, fp) : -EINVAL;
		char b[64];
		struct jw w;
		jw_init(&w, b, sizeof b);
		jw_obj_begin(&w);
		jw_int(&w, "removed", n > 0 ? n : 0);
		jw_obj_end(&w);
		reply(c, &req, cmd, n >= 0, n >= 0 ? NULL : (n == -EINVAL ? "give a fingerprint of at least 4 characters" : "cannot update the list of devices"), &w);
	} else if (!strcmp(cmd, "set_config")) {
		const char *k = json_get(&req, "key"), *v = json_get(&req, "value");
		char err[96] = "";
		int e = (k && v && G.ops.set_config) ? G.ops.set_config(G.ops.user, k, v, err, sizeof err) : -EINVAL;
		reply(c, &req, cmd, e == 0, e == 0 ? NULL : (err[0] ? err : "bad key or value"), NULL);
		if (e == 0) { pthread_mutex_lock(&G.mu); G.st_dirty = true; pthread_mutex_unlock(&G.mu); }
	} else {
		reply(c, &req, cmd, false, "unknown command", NULL);
	}
}

static void drop_client(struct client *c)
{
	if (c->fd >= 0) close(c->fd);
	c->fd = -1;
	c->len = 0;
	pthread_mutex_lock(&G.mu);
	int n = 0;
	for (int i = 0; i < MAX_CLIENTS; i++) if (G.cl[i].fd >= 0 && G.cl[i].pairing) n++;
	G.pairing_clients = n;
	pthread_mutex_unlock(&G.mu);
}

static void announce_pairing(void)
{
	char b[512];
	struct jw w;
	pthread_mutex_lock(&G.mu);
	jw_init(&w, b, sizeof b);
	jw_obj_begin(&w);
	jw_str(&w, "event", "pairing");
	jw_int(&w, "id", G.pair_id);
	jw_str(&w, "name", G.pair_name);
	jw_str(&w, "fingerprint", G.pair_fp);
	char code[16];
	snprintf(code, sizeof code, "%06u", G.pair_code);
	jw_str(&w, "code", code);
	jw_obj_end(&w);
	pthread_mutex_unlock(&G.mu);
	if (jw_done(&w)) broadcast(b, true);
}

static void *thread_main(void *arg)
{
	(void)arg;
	char last[2048] = "";
	struct timespec last_sent = { 0, 0 };
	while (!G.quit) {
		struct pollfd p[2 + MAX_CLIENTS];
		int idx[MAX_CLIENTS], n = 0;
		p[n++] = (struct pollfd){ .fd = G.lfd, .events = POLLIN };
		p[n++] = (struct pollfd){ .fd = G.wake[0], .events = POLLIN };
		for (int i = 0; i < MAX_CLIENTS; i++)
			if (G.cl[i].fd >= 0) { idx[n - 2] = i; p[n++] = (struct pollfd){ .fd = G.cl[i].fd, .events = POLLIN }; }
		poll(p, (nfds_t)n, 500);
		if (G.quit) break;
		if (p[1].revents & POLLIN) { char t[64]; while (read(G.wake[0], t, sizeof t) > 0) {} }
		if (p[0].revents & POLLIN) {
			int fd = accept4(G.lfd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
			if (fd >= 0) {
				struct ucred cr;
				socklen_t l = sizeof cr;
				int slot = -1;
				for (int i = 0; i < MAX_CLIENTS && slot < 0; i++) if (G.cl[i].fd < 0) slot = i;
				if (slot < 0 || getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cr, &l) != 0 || cr.uid != geteuid()) close(fd); /* only this user, and not too many */
				else {
					G.cl[slot] = (struct client){ .fd = fd };
					char b[160];
					struct jw w;
					jw_init(&w, b, sizeof b);
					jw_obj_begin(&w); jw_str(&w, "event", "hello"); jw_str(&w, "version", "1"); jw_obj_end(&w);
					if (jw_done(&w)) send_line(&G.cl[slot], b);
					send_state(&G.cl[slot]);
				}
			}
		}
		for (int k = 0; k < n - 2; k++) {
			struct client *c = &G.cl[idx[k]];
			short re = p[k + 2].revents;
			if (re & POLLIN) {
				ssize_t r = recv(c->fd, c->buf + c->len, sizeof c->buf - c->len - 1, 0);
				if (r <= 0 && errno != EAGAIN && errno != EINTR) { drop_client(c); continue; }
				if (r > 0) c->len += (size_t)r;
				char *nl;
				c->buf[c->len] = 0;
				while ((nl = memchr(c->buf, '\n', c->len))) {
					*nl = 0;
					handle_line(c, c->buf);
					size_t used = (size_t)(nl - c->buf) + 1;
					memmove(c->buf, c->buf + used, c->len - used);
					c->len -= used;
					c->buf[c->len] = 0;
				}
				if (c->len >= sizeof c->buf - 1) drop_client(c); /* a line that never ends */
			} else if (re & (POLLHUP | POLLERR)) drop_client(c);
		}

		bool announce = false;
		pthread_mutex_lock(&G.mu);
		if (G.pair_announce) { announce = true; G.pair_announce = false; }
		char done[160] = "";
		if (G.pair_done_pending) { snprintf(done, sizeof done, "%s", G.pair_done_msg); G.pair_done_pending = false; }
		bool dirty = G.st_dirty;
		struct ctl_state s = G.st;
		bool valid = G.st_valid;
		pthread_mutex_unlock(&G.mu);
		if (announce) announce_pairing();
		if (done[0]) broadcast(done, true);
		if (valid) {
			char b[2048];
			const char *j = state_json(b, sizeof b, &s);
			struct timespec now;
			clock_gettime(CLOCK_MONOTONIC, &now);
			double since = (double)(now.tv_sec - last_sent.tv_sec) + (double)(now.tv_nsec - last_sent.tv_nsec) / 1e9;
			/* send when something changed, at most four times a second; live numbers move every time, so unchanged text is skipped */
			if (j && strcmp(j, last) && (since >= 0.25 || dirty)) {
				broadcast(j, false);
				snprintf(last, sizeof last, "%s", j);
				last_sent = now;
				pthread_mutex_lock(&G.mu);
				G.st_dirty = false;
				pthread_mutex_unlock(&G.mu);
			}
		}
	}
	return NULL;
}

int control_start(const char *path, const struct control_ops *ops)
{
	if (G.running) return -EALREADY;
	if (strlen(path) >= sizeof G.path) return -ENAMETOOLONG;
	snprintf(G.path, sizeof G.path, "%s", path);
	G.ops = *ops;
	/* the directory is private to the user */
	char dir[sizeof G.path];
	snprintf(dir, sizeof dir, "%s", path);
	char *slash = strrchr(dir, '/');
	if (slash && slash != dir) { *slash = 0; mkdir(dir, 0700); chmod(dir, 0700); }
	{ /* another daemon of this user is listening there: leave its socket alone (a test daemon must not take over the real one) */
		int probe = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
		struct sockaddr_un pa = { .sun_family = AF_UNIX };
		snprintf(pa.sun_path, sizeof pa.sun_path, "%s", path);
		int alive = probe >= 0 && connect(probe, (struct sockaddr *)&pa, sizeof pa) == 0;
		if (probe >= 0) close(probe);
		if (alive) return -EADDRINUSE;
	}
	unlink(path); /* a leftover from a crashed daemon */
	G.lfd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (G.lfd < 0) return -errno;
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	snprintf(a.sun_path, sizeof a.sun_path, "%s", path);
	mode_t old = umask(0177);
	int rc = bind(G.lfd, (struct sockaddr *)&a, sizeof a);
	umask(old);
	if (rc < 0 || listen(G.lfd, 4) < 0) { int e = errno; close(G.lfd); G.lfd = -1; return -e; }
	if (pipe2(G.wake, O_CLOEXEC | O_NONBLOCK) != 0) { int e = errno; close(G.lfd); G.lfd = -1; unlink(path); return -e; }
	for (int i = 0; i < MAX_CLIENTS; i++) G.cl[i].fd = -1;
	G.quit = false;
	if (pthread_create(&G.thread, NULL, thread_main, NULL) != 0) { close(G.lfd); close(G.wake[0]); close(G.wake[1]); G.lfd = -1; unlink(path); return -EAGAIN; }
	G.running = true;
	return 0;
}

void control_stop(void)
{
	if (!G.running) return;
	G.quit = true;
	pthread_mutex_lock(&G.mu);
	G.pair_answered = true; /* let a waiting pairing request go */
	G.pair_answer = 'n';
	pthread_cond_broadcast(&G.cv);
	pthread_mutex_unlock(&G.mu);
	wake();
	pthread_join(G.thread, NULL);
	for (int i = 0; i < MAX_CLIENTS; i++) if (G.cl[i].fd >= 0) close(G.cl[i].fd);
	close(G.lfd);
	close(G.wake[0]);
	close(G.wake[1]);
	unlink(G.path);
	G.running = false;
}

void control_publish(const struct ctl_state *s)
{
	if (!G.running) return;
	pthread_mutex_lock(&G.mu);
	bool changed = !G.st_valid || s->session != G.st.session || s->monitor != G.st.monitor || strcmp(s->device, G.st.device) || strcmp(s->usb_state, G.st.usb_state) ||
		       s->grace != G.st.grace || s->width != G.st.width || s->height != G.st.height;
	G.st = *s;
	G.st_valid = true;
	if (changed) G.st_dirty = true;
	pthread_mutex_unlock(&G.mu);
	if (changed) wake();
}

int control_pair_ask(const char *name, const char *fingerprint, unsigned code, int timeout_ms, const volatile sig_atomic_t *stop)
{
	if (!G.running) return -2;
	pthread_mutex_lock(&G.mu);
	if (G.pairing_clients == 0 || G.pair_pending) { pthread_mutex_unlock(&G.mu); return -2; }
	static unsigned next_id = 1;
	G.pair_id = next_id++;
	G.pair_code = code;
	snprintf(G.pair_name, sizeof G.pair_name, "%s", name);
	snprintf(G.pair_fp, sizeof G.pair_fp, "%s", fingerprint);
	G.pair_pending = true;
	G.pair_answered = false;
	G.pair_announce = true;
	unsigned id = G.pair_id;
	pthread_mutex_unlock(&G.mu);
	wake();

	struct timespec t0;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	int result = -1;
	pthread_mutex_lock(&G.mu);
	for (;;) {
		if (G.pair_answered) { result = G.pair_answer; break; }
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		long waited = (now.tv_sec - t0.tv_sec) * 1000 + (now.tv_nsec - t0.tv_nsec) / 1000000;
		if (waited >= timeout_ms || (stop && *stop)) break;
		if (G.pairing_clients == 0 && waited > 1000) { result = -2; break; } /* the GUI went away while asking: let the caller use its own prompt */
		struct timespec until;
		clock_gettime(CLOCK_REALTIME, &until);
		until.tv_nsec += 100 * 1000000;
		if (until.tv_nsec >= 1000000000) { until.tv_sec++; until.tv_nsec -= 1000000000; }
		pthread_cond_timedwait(&G.cv, &G.mu, &until);
	}
	G.pair_pending = false;
	pthread_mutex_unlock(&G.mu);

	char b[160];
	struct jw w;
	jw_init(&w, b, sizeof b);
	jw_obj_begin(&w);
	jw_str(&w, "event", "pairing_done");
	jw_int(&w, "id", id);
	jw_str(&w, "result", result == 'y' ? "always" : result == 'o' ? "once" : result == 'n' ? "deny" : "timeout");
	jw_obj_end(&w);
	if (jw_done(&w)) {
		pthread_mutex_lock(&G.mu);
		snprintf(G.pair_done_msg, sizeof G.pair_done_msg, "%s", b);
		G.pair_done_pending = true;
		pthread_mutex_unlock(&G.mu);
		wake();
	}
	return result;
}
