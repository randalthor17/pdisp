#include "json.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- writer */

static void put(struct jw *w, const char *s, size_t n)
{
	if (w->overflow) return;
	if (w->len + n + 1 > w->cap) { w->overflow = true; return; }
	memcpy(w->buf + w->len, s, n);
	w->len += n;
	w->buf[w->len] = 0;
}
static void puts_(struct jw *w, const char *s) { put(w, s, strlen(s)); }

static void put_escaped(struct jw *w, const char *s)
{
	put(w, "\"", 1);
	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;
		char t[8];
		switch (c) {
		case '"': puts_(w, "\\\""); break;
		case '\\': puts_(w, "\\\\"); break;
		case '\n': puts_(w, "\\n"); break;
		case '\r': puts_(w, "\\r"); break;
		case '\t': puts_(w, "\\t"); break;
		default:
			if (c < 0x20) { snprintf(t, sizeof t, "\\u%04x", c); puts_(w, t); }
			else put(w, (const char *)&c, 1);
		}
	}
	put(w, "\"", 1);
}

static void key_(struct jw *w, const char *key)
{
	if (w->need_comma) put(w, ",", 1);
	w->need_comma = true;
	if (key) { put_escaped(w, key); put(w, ":", 1); }
}

void jw_init(struct jw *w, char *buf, size_t cap)
{
	memset(w, 0, sizeof *w);
	w->buf = buf;
	w->cap = cap;
	if (cap) buf[0] = 0;
}
void jw_obj_begin(struct jw *w) { key_(w, NULL); put(w, "{", 1); w->need_comma = false; }
void jw_obj_member(struct jw *w, const char *key) { key_(w, key); put(w, "{", 1); w->need_comma = false; }
void jw_obj_end(struct jw *w) { put(w, "}", 1); w->need_comma = true; }
void jw_arr_begin(struct jw *w, const char *key) { key_(w, key); put(w, "[", 1); w->need_comma = false; }
void jw_arr_end(struct jw *w) { put(w, "]", 1); w->need_comma = true; }
void jw_str(struct jw *w, const char *key, const char *value) { key_(w, key); put_escaped(w, value ? value : ""); }
void jw_int(struct jw *w, const char *key, long long value) { char t[32]; key_(w, key); snprintf(t, sizeof t, "%lld", value); puts_(w, t); }
void jw_num(struct jw *w, const char *key, double value) { char t[48]; key_(w, key); snprintf(t, sizeof t, "%.3f", value); puts_(w, t); }
void jw_bool(struct jw *w, const char *key, bool value) { key_(w, key); puts_(w, value ? "true" : "false"); }
void jw_null(struct jw *w, const char *key) { key_(w, key); puts_(w, "null"); }
const char *jw_done(struct jw *w) { return w->overflow ? NULL : w->buf; }

/* ---- parser */

static const char *skip(const char *p) { while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++; return p; }

static const char *parse_string(const char *p, char *out, size_t cap)
{
	if (*p != '"') return NULL;
	p++;
	size_t n = 0;
	for (; *p && *p != '"'; p++) {
		char c = *p;
		if (c == '\\') {
			p++;
			switch (*p) {
			case 'n': c = '\n'; break;
			case 't': c = '\t'; break;
			case 'r': c = '\r'; break;
			case '"': case '\\': case '/': c = *p; break;
			case 'u': { /* only the escapes a control character needs; anything else becomes '?' */
				unsigned v = 0;
				for (int i = 1; i <= 4; i++) { if (!isxdigit((unsigned char)p[i])) return NULL; v = v * 16 + (unsigned)(isdigit((unsigned char)p[i]) ? p[i] - '0' : (tolower((unsigned char)p[i]) - 'a' + 10)); }
				p += 4;
				c = v < 0x80 ? (char)v : '?';
				break;
			}
			default: return NULL;
			}
		}
		if (n + 1 >= cap) return NULL;
		out[n++] = c;
	}
	if (*p != '"') return NULL;
	out[n] = 0;
	return p + 1;
}

int json_parse_flat(const char *text, struct jobj *out)
{
	memset(out, 0, sizeof *out);
	const char *p = skip(text);
	if (*p != '{') return -1;
	p = skip(p + 1);
	if (*p == '}') return 0;
	for (;;) {
		if (out->n >= JSON_MAX_FIELDS) return -1;
		struct jfield *f = &out->f[out->n];
		p = parse_string(skip(p), f->key, sizeof f->key);
		if (!p) return -1;
		p = skip(p);
		if (*p != ':') return -1;
		p = skip(p + 1);
		if (*p == '"') {
			f->is_string = true;
			p = parse_string(p, f->str, sizeof f->str);
			if (!p) return -1;
		} else {
			size_t n = 0;
			while (*p && *p != ',' && *p != '}' && !isspace((unsigned char)*p)) {
				if (*p == '{' || *p == '[') return -1; /* nested values are not commands */
				if (n + 1 >= sizeof f->str) return -1;
				f->str[n++] = *p++;
			}
			f->str[n] = 0;
			if (!n) return -1;
		}
		out->n++;
		p = skip(p);
		if (*p == ',') { p++; continue; }
		if (*p == '}') return *skip(p + 1) ? -1 : 0;
		return -1;
	}
}

const char *json_get(const struct jobj *o, const char *key)
{
	for (int i = 0; i < o->n; i++) if (!strcmp(o->f[i].key, key)) return o->f[i].str;
	return NULL;
}
long long json_get_int(const struct jobj *o, const char *key, long long dflt)
{
	const char *v = json_get(o, key);
	if (!v || !*v) return dflt;
	char *end;
	long long r = strtoll(v, &end, 10);
	return *end ? dflt : r;
}
bool json_get_bool(const struct jobj *o, const char *key, bool dflt)
{
	const char *v = json_get(o, key);
	if (!v) return dflt;
	if (!strcmp(v, "true")) return true;
	if (!strcmp(v, "false")) return false;
	return dflt;
}
