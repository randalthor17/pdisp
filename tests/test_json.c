#include <stdio.h>
#include <string.h>

#include "json.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

int main(void)
{
	char buf[512];
	struct jw w;
	jw_init(&w, buf, sizeof buf);
	jw_obj_begin(&w);
	jw_str(&w, "event", "state");
	jw_int(&w, "port", 47110);
	jw_num(&w, "fps", 59.9996);
	jw_bool(&w, "usb", true);
	jw_null(&w, "session");
	jw_str(&w, "name", "a \"quoted\"\nname\\");
	jw_arr_begin(&w, "devices");
	jw_obj_begin(&w); jw_str(&w, "key", "ab"); jw_obj_end(&w);
	jw_obj_begin(&w); jw_str(&w, "key", "cd"); jw_obj_end(&w);
	jw_arr_end(&w);
	jw_obj_member(&w, "monitor"); jw_str(&w, "connector", "DVI-I-1"); jw_obj_end(&w);
	jw_obj_end(&w);
	const char *s = jw_done(&w);
	CHECK(s != NULL);
	CHECK(s && !strcmp(s, "{\"event\":\"state\",\"port\":47110,\"fps\":60.000,\"usb\":true,\"session\":null,\"name\":\"a \\\"quoted\\\"\\nname\\\\\","
			       "\"devices\":[{\"key\":\"ab\"},{\"key\":\"cd\"}],\"monitor\":{\"connector\":\"DVI-I-1\"}}"));

	char tiny[16];
	jw_init(&w, tiny, sizeof tiny);
	jw_obj_begin(&w); jw_str(&w, "k", "a long value that does not fit"); jw_obj_end(&w);
	CHECK(jw_done(&w) == NULL);

	struct jobj o;
	CHECK(json_parse_flat("{\"cmd\":\"pair_answer\", \"id\": 7, \"answer\":\"once\",\"x\":true}", &o) == 0);
	CHECK(!strcmp(json_get(&o, "cmd"), "pair_answer"));
	CHECK(json_get_int(&o, "id", -1) == 7);
	CHECK(!strcmp(json_get(&o, "answer"), "once"));
	CHECK(json_get_bool(&o, "x", false));
	CHECK(json_get(&o, "nothing") == NULL);
	CHECK(json_get_int(&o, "answer", 9) == 9); /* not a number */
	CHECK(json_parse_flat("{\"s\":\"line\\nbreak \\\"q\\\" \\u0041\"}", &o) == 0 && !strcmp(json_get(&o, "s"), "line\nbreak \"q\" A"));
	CHECK(json_parse_flat("{}", &o) == 0 && o.n == 0);
	CHECK(json_parse_flat("{\"a\":{\"b\":1}}", &o) == -1);   /* nested */
	CHECK(json_parse_flat("{\"a\":[1]}", &o) == -1);
	CHECK(json_parse_flat("{\"a\":1", &o) == -1);            /* unterminated */
	CHECK(json_parse_flat("{\"a\":1} x", &o) == -1);         /* trailing text */
	CHECK(json_parse_flat("not json", &o) == -1);
	CHECK(json_parse_flat("{\"a\":\"unterminated}", &o) == -1);
	char big[300];
	memset(big, 'x', sizeof big);
	big[sizeof big - 1] = 0;
	char line[400];
	snprintf(line, sizeof line, "{\"a\":\"%s\"}", big);
	CHECK(json_parse_flat(line, &o) == -1);                  /* a value longer than a field holds */

	if (failures) { fprintf(stderr, "%d check(s) failed\n", failures); return 1; }
	puts("json: all checks passed");
	return 0;
}
