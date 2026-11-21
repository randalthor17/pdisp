/* A device identity (an X25519 key pair kept in a file) and key stores (the daemon's authorized_keys, the phone's known PCs).
 * See docs/security.md. Files are created private (0600 in a 0700 directory); a secret key file that other users can read is refused. */
#ifndef PDISP_IDENTITY_H
#define PDISP_IDENTITY_H

#include <stddef.h>
#include <stdint.h>

#include "crypto.h"

struct identity {
	uint8_t secret[PC_DH_LEN];
	uint8_t pub[PC_DH_LEN];
};

#define IDENTITY_FINGERPRINT_LEN 25 /* "xxxx-xxxx-xxxx-xxxx-xxxx" and the terminating NUL */

/* The default directory for this user: $XDG_CONFIG_HOME/pdisp, else $HOME/.config/pdisp. Returns 0 or -ENOENT. */
int identity_default_dir(char *out, size_t cap);

/* Loads `<dir>/identity`, creating the directory and a fresh key if it does not exist. `*created` (if not NULL) says which.
 * Returns 0, -EACCES (the file is readable by others), -EBADMSG (not a valid key file) or another negative errno. */
int identity_load_or_create(const char *dir, struct identity *id, int *created);

/* A short form of a public key for people to compare: 80 bits of its hash in hex groups. */
void identity_fingerprint(const uint8_t pub[PC_DH_LEN], char out[IDENTITY_FINGERPRINT_LEN]);

/* Key stores: text files, one key per line: 64 hex digits, a space, an optional name. Lines starting with # and malformed lines
 * are ignored. */
struct keystore_entry {
	uint8_t pub[PC_DH_LEN];
	char name[64];
};
typedef void (*keystore_visit)(const struct keystore_entry *e, void *user);

/* 1 if the key is in the file (its stored name goes to `name`, if given), 0 if not (or the file does not exist), or -errno. */
int keystore_find(const char *path, const uint8_t pub[PC_DH_LEN], char *name, size_t name_cap);
/* Adds the key unless it is already there. Creates the file private. Returns 0 or -errno. */
int keystore_add(const char *path, const uint8_t pub[PC_DH_LEN], const char *name);
/* Removes every key whose fingerprint starts with `fingerprint_prefix` (at least 4 characters). Returns how many, or -errno. */
int keystore_remove(const char *path, const char *fingerprint_prefix);
int keystore_list(const char *path, keystore_visit visit, void *user);

#endif
