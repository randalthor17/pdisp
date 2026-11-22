/* A very small JSON helper for the control socket (docs/control.md): a writer that appends to a buffer, and a parser for one flat object of
 * strings, numbers and booleans, which is all a command is. No other dependency. */
#ifndef PDISP_JSON_H
#define PDISP_JSON_H

#include <stdbool.h>
#include <stddef.h>

struct jw {
	char *buf;
	size_t cap, len;
	bool overflow;
	bool need_comma; /* a value was written at this nesting level */
};

void jw_init(struct jw *w, char *buf, size_t cap);
void jw_obj_begin(struct jw *w);          /* { */
void jw_obj_end(struct jw *w);            /* } */
void jw_arr_begin(struct jw *w, const char *key); /* "key":[ (key may be NULL inside an array) */
void jw_arr_end(struct jw *w);            /* ] */
void jw_obj_member(struct jw *w, const char *key); /* "key":{ opens an object member */
void jw_str(struct jw *w, const char *key, const char *value);
void jw_int(struct jw *w, const char *key, long long value);
void jw_num(struct jw *w, const char *key, double value);
void jw_bool(struct jw *w, const char *key, bool value);
void jw_null(struct jw *w, const char *key);
/* the finished text (NUL terminated), or NULL if the buffer was too small */
const char *jw_done(struct jw *w);

#define JSON_MAX_FIELDS 16
struct jfield {
	char key[32];
	char str[128]; /* the value as text: a string's contents, or the number or word as written */
	bool is_string;
};
struct jobj {
	struct jfield f[JSON_MAX_FIELDS];
	int n;
};

/* Parses one flat JSON object: {"a":"text","b":12,"c":true}. Returns 0, or -1 if it is not that (nested values, bad syntax, too long). */
int json_parse_flat(const char *text, struct jobj *out);
const char *json_get(const struct jobj *o, const char *key);       /* the value text, or NULL */
long long json_get_int(const struct jobj *o, const char *key, long long dflt);
bool json_get_bool(const struct jobj *o, const char *key, bool dflt);

#endif
