/* Unit tests for the codec sets: the setting's text, what the phone offers, and the order the PC prefers. */
#include "codecs.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond)                                                                                  \
	do {                                                                                         \
		if (!(cond)) {                                                                       \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);     \
			failures++;                                                                  \
		}                                                                                    \
	} while (0)

#define HEVC (1u << PROTO_CODEC_HEVC)
#define H264 (1u << PROTO_CODEC_H264)

int main(void)
{
	/* parsing the setting */
	CHECK(codec_set_parse("hevc") == HEVC);
	CHECK(codec_set_parse("h264") == H264);
	CHECK(codec_set_parse("hevc,h264") == (HEVC | H264));
	CHECK(codec_set_parse("h264,hevc") == (HEVC | H264));
	CHECK(codec_set_parse("HEVC, H264") == (HEVC | H264));
	CHECK(codec_set_parse("h265") == HEVC && codec_set_parse("avc") == H264);
	CHECK(codec_set_parse("") == 0);
	CHECK(codec_set_parse(",") == 0);
	CHECK(codec_set_parse("av1") == 0);          /* not something the PC can encode */
	CHECK(codec_set_parse("hevc,vp9") == 0);     /* one bad word spoils the list */
	CHECK(codec_set_parse("hevchevchevchevchevc") == 0); /* a long word is not truncated into a valid one */
	CHECK(CODEC_SET_ALL == (HEVC | H264));

	/* writing it back, in preference order whatever the input order */
	char buf[32];
	codec_set_format(HEVC | H264, buf, sizeof buf); CHECK(!strcmp(buf, "hevc,h264"));
	codec_set_format(H264, buf, sizeof buf); CHECK(!strcmp(buf, "h264"));
	codec_set_format(HEVC, buf, sizeof buf); CHECK(!strcmp(buf, "hevc"));
	codec_set_format(0, buf, sizeof buf); CHECK(!strcmp(buf, ""));
	codec_set_format(HEVC | H264, buf, 6); CHECK(!strcmp(buf, "hevc,")); /* truncated, still terminated */
	char round[32];
	codec_set_format(codec_set_parse("h264,hevc"), round, sizeof round);
	CHECK(codec_set_parse(round) == (HEVC | H264));

	/* the phone's list: only the codecs the PC can encode count */
	const uint8_t both[] = { PROTO_CODEC_HEVC, PROTO_CODEC_H264, PROTO_CODEC_AV1 };
	CHECK(codec_set_from_list(both, 3) == (HEVC | H264));
	const uint8_t only264[] = { PROTO_CODEC_H264 };
	CHECK(codec_set_from_list(only264, 1) == H264);
	const uint8_t none[] = { PROTO_CODEC_AV1, PROTO_CODEC_HEVC_MAIN10, 99 };
	CHECK(codec_set_from_list(none, 3) == 0);
	CHECK(codec_set_from_list(both, 0) == 0);

	/* the choice: HEVC when both sides have it, else H.264, and the next one to try */
	CHECK(codec_best(HEVC | H264) == PROTO_CODEC_HEVC);
	CHECK(codec_best(H264) == PROTO_CODEC_H264);
	CHECK(codec_best(HEVC) == PROTO_CODEC_HEVC);
	CHECK(codec_best(0) == 0);
	CHECK(codec_next(HEVC | H264, PROTO_CODEC_HEVC) == PROTO_CODEC_H264);
	CHECK(codec_next(HEVC | H264, PROTO_CODEC_H264) == 0);
	CHECK(codec_next(HEVC, PROTO_CODEC_HEVC) == 0);
	CHECK(codec_next(H264, PROTO_CODEC_HEVC) == PROTO_CODEC_H264);
	/* the PC's setting and the phone's list together */
	CHECK(codec_best(codec_set_parse("h264") & codec_set_from_list(both, 3)) == PROTO_CODEC_H264);
	CHECK(codec_best(codec_set_parse("hevc") & codec_set_from_list(only264, 1)) == 0);

	CHECK(!strcmp(codec_name(PROTO_CODEC_HEVC), "hevc") && !strcmp(codec_name(PROTO_CODEC_H264), "h264") && !strcmp(codec_name(7), "?"));

	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	puts("codecs: all checks passed");
	return 0;
}
