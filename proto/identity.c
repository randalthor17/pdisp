#include "identity.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void to_hex(const uint8_t *in, size_t n, char *out)
{
	static const char d[] = "0123456789abcdef";
	for (size_t i = 0; i < n; i++) { out[2 * i] = d[in[i] >> 4]; out[2 * i + 1] = d[in[i] & 15]; }
	out[2 * n] = 0;
}

static int from_hex(const char *s, size_t n, uint8_t *out)
{
	for (size_t i = 0; i < n; i++) {
		int hi = isxdigit((unsigned char)s[2 * i]) ? 1 : 0, lo = isxdigit((unsigned char)s[2 * i + 1]) ? 1 : 0;
		if (!hi || !lo) return -1;
		unsigned v;
		char t[3] = { s[2 * i], s[2 * i + 1], 0 };
		sscanf(t, "%2x", &v);
		out[i] = (uint8_t)v;
	}
	return 0;
}

int identity_default_dir(char *out, size_t cap)
{
	const char *x = getenv("XDG_CONFIG_HOME"), *h = getenv("HOME");
	int n;
	if (x && *x) n = snprintf(out, cap, "%s/pdisp", x);
	else if (h && *h) n = snprintf(out, cap, "%s/.config/pdisp", h);
	else return -ENOENT;
	return n > 0 && (size_t)n < cap ? 0 : -ENAMETOOLONG;
}

static int make_dir(const char *dir)
{
	/* create each missing level; only the last one is ours to make private */
	char tmp[1024];
	size_t n = strlen(dir);
	if (n == 0 || n >= sizeof tmp) return -ENAMETOOLONG;
	memcpy(tmp, dir, n + 1);
	for (size_t i = 1; i < n; i++)
		if (tmp[i] == '/') { tmp[i] = 0; if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -errno; tmp[i] = '/'; }
	if (mkdir(tmp, 0700) != 0 && errno != EEXIST) return -errno;
	return 0;
}

int identity_load_or_create(const char *dir, struct identity *id, int *created)
{
	char path[1100];
	if (snprintf(path, sizeof path, "%s/identity", dir) >= (int)sizeof path) return -ENAMETOOLONG;
	if (created) *created = 0;
	int err = make_dir(dir);
	if (err) return err;

	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0 && errno == ENOENT) {
		uint8_t secret[PC_DH_LEN];
		if (pc_random(secret, sizeof secret) != 0) return -EIO;
		fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
		if (fd < 0 && errno == EEXIST) goto reopen; /* another process created it a moment ago */
		if (fd < 0) return -errno;
		char hex[2 * PC_DH_LEN + 2];
		to_hex(secret, sizeof secret, hex);
		hex[2 * PC_DH_LEN] = '\n';
		ssize_t w = write(fd, hex, sizeof hex - 1);
		int werr = w == (ssize_t)(sizeof hex - 1) ? 0 : -EIO;
		close(fd);
		pc_wipe(hex, sizeof hex);
		if (werr) { unlink(path); pc_wipe(secret, sizeof secret); return werr; }
		memcpy(id->secret, secret, PC_DH_LEN);
		pc_wipe(secret, sizeof secret);
		pc_x25519_public(id->pub, id->secret);
		if (created) *created = 1;
		return 0;
	}
reopen:
	if (fd < 0) fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) return -errno;
	struct stat st;
	if (fstat(fd, &st) != 0) { err = -errno; close(fd); return err; }
	if (st.st_mode & 077) { close(fd); return -EACCES; } /* a secret that others can read is not a secret */
	char buf[200];
	ssize_t n = read(fd, buf, sizeof buf - 1);
	close(fd);
	if (n < 2 * PC_DH_LEN) return -EBADMSG;
	buf[n] = 0;
	if (from_hex(buf, PC_DH_LEN, id->secret) != 0) { pc_wipe(buf, sizeof buf); return -EBADMSG; }
	for (ssize_t i = 2 * PC_DH_LEN; i < n; i++) if (!isspace((unsigned char)buf[i])) { pc_wipe(buf, sizeof buf); return -EBADMSG; }
	pc_wipe(buf, sizeof buf);
	pc_x25519_public(id->pub, id->secret);
	return 0;
}

void identity_fingerprint(const uint8_t pub[PC_DH_LEN], char out[IDENTITY_FINGERPRINT_LEN])
{
	uint8_t h[PC_HASH_LEN];
	pc_hash(h, pub, PC_DH_LEN);
	char hex[21];
	to_hex(h, 10, hex);
	snprintf(out, IDENTITY_FINGERPRINT_LEN, "%.4s-%.4s-%.4s-%.4s-%.4s", hex, hex + 4, hex + 8, hex + 12, hex + 16);
}

/* ---- key stores */

static int parse_line(char *line, struct keystore_entry *e)
{
	while (isspace((unsigned char)*line)) line++;
	if (*line == '#' || *line == 0) return -1;
	if (strlen(line) < 2 * PC_DH_LEN || from_hex(line, PC_DH_LEN, e->pub) != 0) return -1;
	char *p = line + 2 * PC_DH_LEN;
	if (*p && !isspace((unsigned char)*p)) return -1;
	while (isspace((unsigned char)*p)) p++;
	size_t n = strlen(p);
	while (n && isspace((unsigned char)p[n - 1])) p[--n] = 0;
	snprintf(e->name, sizeof e->name, "%s", p);
	return 0;
}

static int each_entry(const char *path, int (*fn)(const struct keystore_entry *, void *), void *user)
{
	FILE *f = fopen(path, "r");
	if (!f) return errno == ENOENT ? 0 : -errno;
	char line[512];
	int stop = 0;
	while (!stop && fgets(line, sizeof line, f)) {
		struct keystore_entry e;
		if (parse_line(line, &e) == 0) stop = fn(&e, user);
	}
	fclose(f);
	return stop < 0 ? stop : 0;
}

struct find_ctx { const uint8_t *pub; char *name; size_t cap; int found; };
static int find_cb(const struct keystore_entry *e, void *u)
{
	struct find_ctx *c = u;
	if (pc_ct_compare(e->pub, c->pub, PC_DH_LEN) != 0) return 0;
	c->found = 1;
	if (c->name && c->cap) snprintf(c->name, c->cap, "%s", e->name);
	return 1;
}

int keystore_find(const char *path, const uint8_t pub[PC_DH_LEN], char *name, size_t name_cap)
{
	struct find_ctx c = { pub, name, name_cap, 0 };
	int r = each_entry(path, find_cb, &c);
	return r < 0 ? r : c.found;
}

int keystore_add(const char *path, const uint8_t pub[PC_DH_LEN], const char *name)
{
	int r = keystore_find(path, pub, NULL, 0);
	if (r != 0) return r < 0 ? r : 0;
	char hex[2 * PC_DH_LEN + 1], clean[64];
	to_hex(pub, PC_DH_LEN, hex);
	size_t n = 0;
	for (const char *p = name ? name : ""; *p && n < sizeof clean - 1; p++) clean[n++] = (*p == '\n' || *p == '\r' || *p == '\t') ? ' ' : *p;
	clean[n] = 0;
	int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
	if (fd < 0) return -errno;
	char line[2 * PC_DH_LEN + 1 + 64 + 2];
	int len = snprintf(line, sizeof line, "%s %s\n", hex, clean);
	ssize_t w = write(fd, line, (size_t)len);
	close(fd);
	return w == len ? 0 : -EIO;
}

struct rm_ctx { FILE *out; const char *prefix; size_t plen; int removed; };
static int rm_cb(const struct keystore_entry *e, void *u)
{
	struct rm_ctx *c = u;
	char fp[IDENTITY_FINGERPRINT_LEN];
	identity_fingerprint(e->pub, fp);
	if (strncmp(fp, c->prefix, c->plen) == 0) { c->removed++; return 0; }
	char hex[2 * PC_DH_LEN + 1];
	to_hex(e->pub, PC_DH_LEN, hex);
	fprintf(c->out, "%s %s\n", hex, e->name);
	return 0;
}

int keystore_remove(const char *path, const char *prefix)
{
	size_t plen = strlen(prefix);
	if (plen < 4) return -EINVAL; /* a prefix too short to be specific would remove the wrong key */
	char tmp[1100];
	if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp) return -ENAMETOOLONG;
	int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0) return -errno;
	FILE *out = fdopen(fd, "w");
	if (!out) { close(fd); return -errno; }
	struct rm_ctx c = { out, prefix, plen, 0 };
	int r = each_entry(path, rm_cb, &c);
	if (fclose(out) != 0 && r == 0) r = -EIO;
	if (r == 0 && c.removed) r = rename(tmp, path) == 0 ? 0 : -errno;
	if (r != 0 || !c.removed) unlink(tmp);
	return r < 0 ? r : c.removed;
}

struct list_ctx { keystore_visit visit; void *user; };
static int list_cb(const struct keystore_entry *e, void *u) { struct list_ctx *c = u; c->visit(e, c->user); return 0; }
int keystore_list(const char *path, keystore_visit visit, void *user)
{
	struct list_ctx c = { visit, user };
	return each_entry(path, list_cb, &c);
}
