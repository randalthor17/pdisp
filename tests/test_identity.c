/* identity files and key stores */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "identity.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static void count(const struct keystore_entry *e, void *u) { (void)e; (*(int *)u)++; }
static mode_t mode_of(const char *p) { struct stat st; return stat(p, &st) == 0 ? st.st_mode & 0777 : 0xffff; }
static void write_file(const char *p, const char *s, mode_t m) { int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, m); (void)!write(fd, s, strlen(s)); close(fd); chmod(p, m); }

int main(void)
{
	char base[] = "/tmp/pdisp-identity-XXXXXX";
	CHECK(mkdtemp(base) != NULL);
	char dir[300], path[400];
	snprintf(dir, sizeof dir, "%s/a/b/pdisp", base); /* nested, none of it exists */

	struct identity id1, id2;
	int created = -1;
	CHECK(identity_load_or_create(dir, &id1, &created) == 0 && created == 1);
	snprintf(path, sizeof path, "%s/identity", dir);
	CHECK(mode_of(path) == 0600 && mode_of(dir) == 0700);
	CHECK(identity_load_or_create(dir, &id2, &created) == 0 && created == 0);
	CHECK(memcmp(&id1, &id2, sizeof id1) == 0);
	uint8_t pub[32];
	pc_x25519_public(pub, id1.secret);
	CHECK(memcmp(pub, id1.pub, 32) == 0);

	/* other users must not be able to read it: refuse, do not fix silently */
	chmod(path, 0644);
	CHECK(identity_load_or_create(dir, &id2, NULL) == -EACCES);
	chmod(path, 0640);
	CHECK(identity_load_or_create(dir, &id2, NULL) == -EACCES);
	chmod(path, 0600);
	CHECK(identity_load_or_create(dir, &id2, NULL) == 0);
	/* garbage, short, trailing junk */
	write_file(path, "not a key\n", 0600);
	CHECK(identity_load_or_create(dir, &id2, NULL) == -EBADMSG);
	write_file(path, "0011", 0600);
	CHECK(identity_load_or_create(dir, &id2, NULL) == -EBADMSG);
	char good[200] = "";
	for (int i = 0; i < 32; i++) sprintf(good + 2 * i, "%02x", i);
	char withjunk[300];
	snprintf(withjunk, sizeof withjunk, "%s\nxx\n", good);
	write_file(path, withjunk, 0600);
	CHECK(identity_load_or_create(dir, &id2, NULL) == -EBADMSG);
	strcat(good, "\n");
	write_file(path, good, 0600);
	CHECK(identity_load_or_create(dir, &id2, NULL) == 0 && id2.secret[5] == 5);
	char dd[100];
	setenv("XDG_CONFIG_HOME", "/x/y", 1);
	CHECK(identity_default_dir(dd, sizeof dd) == 0 && strcmp(dd, "/x/y/pdisp") == 0);
	unsetenv("XDG_CONFIG_HOME");
	setenv("HOME", "/home/u", 1);
	CHECK(identity_default_dir(dd, sizeof dd) == 0 && strcmp(dd, "/home/u/.config/pdisp") == 0);

	char fp[IDENTITY_FINGERPRINT_LEN], fp2[IDENTITY_FINGERPRINT_LEN];
	identity_fingerprint(id1.pub, fp);
	identity_fingerprint(id1.pub, fp2);
	CHECK(strlen(fp) == 24 && fp[4] == '-' && fp[9] == '-' && fp[14] == '-' && fp[19] == '-' && strcmp(fp, fp2) == 0);
	identity_fingerprint(id2.pub, fp2);
	CHECK(strcmp(fp, fp2) != 0);

	/* key store */
	char ks[400];
	snprintf(ks, sizeof ks, "%s/authorized_keys", dir);
	uint8_t ka[32], kb[32], kc[32];
	pc_random(ka, 32); pc_random(kb, 32); pc_random(kc, 32);
	char name[64];
	CHECK(keystore_find(ks, ka, name, sizeof name) == 0); /* no file yet: simply not there */
	CHECK(keystore_add(ks, ka, "Pixel 8a") == 0 && mode_of(ks) == 0600);
	CHECK(keystore_add(ks, ka, "Pixel again") == 0); /* idempotent: the first name stays */
	CHECK(keystore_add(ks, kb, "Tab\tS9\nFE with a very long name that goes on and on and on and on and on for much too long") == 0);
	CHECK(keystore_find(ks, ka, name, sizeof name) == 1 && strcmp(name, "Pixel 8a") == 0);
	CHECK(keystore_find(ks, kb, name, sizeof name) == 1 && strchr(name, '\t') == NULL && strchr(name, '\n') == NULL && strlen(name) < 64);
	CHECK(keystore_find(ks, kc, name, sizeof name) == 0);
	int n = 0;
	CHECK(keystore_list(ks, count, &n) == 0 && n == 2);

	/* hand-edited files: comments, blank lines, garbage and CRLF are skipped without hiding real entries */
	FILE *f = fopen(ks, "a");
	char hexc[65];
	for (int i = 0; i < 32; i++) sprintf(hexc + 2 * i, "%02x", kc[i]);
	fprintf(f, "# a comment\n\nnot-hex-at-all\n%.60s\n%s name with spaces  \r\n", hexc, hexc);
	fclose(f);
	CHECK(keystore_find(ks, kc, name, sizeof name) == 1 && strcmp(name, "name with spaces") == 0);
	n = 0;
	keystore_list(ks, count, &n);
	CHECK(n == 3);

	/* removal needs a specific prefix and removes only that key */
	char fpa[IDENTITY_FINGERPRINT_LEN];
	identity_fingerprint(ka, fpa);
	CHECK(keystore_remove(ks, "ab") == -EINVAL);
	CHECK(keystore_remove(ks, "zzzz") == 0);
	CHECK(keystore_remove(ks, fpa) == 1);
	CHECK(keystore_find(ks, ka, NULL, 0) == 0 && keystore_find(ks, kb, NULL, 0) == 1 && keystore_find(ks, kc, NULL, 0) == 1);
	CHECK(mode_of(ks) == 0600);

	snprintf(path, sizeof path, "rm -rf %s", base);
	CHECK(system(path) == 0);
	if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
	puts("identity: all checks passed");
	return 0;
}
