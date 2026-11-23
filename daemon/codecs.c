#include "codecs.h"

#include "proto.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static unsigned bit(int codec) { return codec > 0 && codec < 32 ? 1u << codec : 0; }

const char *codec_name(int codec)
{
	switch (codec) {
	case PROTO_CODEC_HEVC: return "hevc";
	case PROTO_CODEC_H264: return "h264";
	default: return "?";
	}
}

unsigned codec_set_parse(const char *s)
{
	unsigned set = 0;
	while (*s) {
		char word[16];
		size_t n = 0;
		while (*s && *s != ',' && *s != ' ') {
			if (n + 1 < sizeof word) word[n++] = (char)tolower((unsigned char)*s);
			s++;
		}
		word[n] = 0;
		while (*s == ',' || *s == ' ') s++;
		if (!n) continue;
		if (!strcmp(word, "hevc") || !strcmp(word, "h265")) set |= bit(PROTO_CODEC_HEVC);
		else if (!strcmp(word, "h264") || !strcmp(word, "avc")) set |= bit(PROTO_CODEC_H264);
		else return 0;
	}
	return set;
}

void codec_set_format(unsigned set, char *buf, size_t cap)
{
	if (!cap) return;
	buf[0] = 0;
	for (int c = codec_best(set); c; c = codec_next(set, c))
		snprintf(buf + strlen(buf), cap - strlen(buf), "%s%s", buf[0] ? "," : "", codec_name(c));
}

unsigned codec_set_from_list(const uint8_t *codecs, unsigned n)
{
	unsigned set = 0;
	for (unsigned i = 0; i < n; i++)
		if (codecs[i] == PROTO_CODEC_HEVC || codecs[i] == PROTO_CODEC_H264) set |= bit(codecs[i]);
	return set;
}

int codec_best(unsigned set)
{
	return codec_next(set, 0);
}

int codec_next(unsigned set, int after)
{
	static const int order[] = { PROTO_CODEC_HEVC, PROTO_CODEC_H264 };
	unsigned i = 0;
	if (after) {
		while (i < sizeof order / sizeof order[0] && order[i] != after) i++;
		i++;
	}
	for (; i < sizeof order / sizeof order[0]; i++)
		if (set & bit(order[i])) return order[i];
	return 0;
}
