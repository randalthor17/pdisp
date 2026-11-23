/* Which video codecs a session may use: the PC's setting ("hevc,h264"), what the phone offers, and the order the PC prefers.
 * A set is a bit mask with bit PROTO_CODEC_* (proto.h) set for every member. Only HEVC and H.264 can be encoded. */
#ifndef PDISP_CODECS_H
#define PDISP_CODECS_H

#include <stddef.h>
#include <stdint.h>

#include "proto.h"

#define CODEC_SET_ALL ((1u << PROTO_CODEC_HEVC) | (1u << PROTO_CODEC_H264))

/* "hevc", "h264", "hevc,h264" (any order, case-insensitive, "avc" for h264, "h265" for hevc) to a set. Returns 0 for an empty or invalid list. */
unsigned codec_set_parse(const char *s);

/* The set as "hevc,h264" (preference order). `buf` always ends in a NUL. */
void codec_set_format(unsigned set, char *buf, size_t cap);

/* The codecs of a HELLO codec list that the PC can encode, as a set. */
unsigned codec_set_from_list(const uint8_t *codecs, unsigned n);

/* The best codec in `set`: HEVC (more efficient) before H.264. Returns its PROTO_CODEC_* value, or 0 if the set is empty. */
int codec_best(unsigned set);

/* The next best after `after` (an earlier result of codec_best or this), or 0. Used to try another encoder when one cannot be opened. */
int codec_next(unsigned set, int after);

const char *codec_name(int codec); /* "hevc", "h264", or "?" */

#endif
