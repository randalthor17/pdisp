/* pdisp wire format (docs/protocol.md): bounds-checked encoders and decoders for the control messages and UDP packets.
 *
 * Conventions: little-endian, no padding. Every decoder validates all lengths against the buffer, never reads past it,
 * and returns 0 on success or a negative errno (-EINVAL malformed or truncated, -EMSGSIZE a field over its limit).
 * Every encoder returns the number of bytes written, or a negative errno (-EMSGSIZE if the buffer is too small, -EINVAL
 * if a field is out of range). Unknown trailing TLVs are skipped by the decoders and not produced by the encoders.
 */
#ifndef PDISP_PROTO_H
#define PDISP_PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PROTO_VERSION 1
#define PROTO_MAX_CTL_PAYLOAD (1u << 20)

#define PROTO_NAME_MAX 64       /* device_name, including the terminating NUL */
#define PROTO_VERSION_STR_MAX 32
#define PROTO_TEXT_MAX 128      /* REJECT and CLOSE text */
#define PROTO_MAX_RATES 16
#define PROTO_MAX_CODECS 8
#define PROTO_MAX_PARAM_SETS 1024
#define PROTO_MAX_CURSOR_DIM 128
#define PROTO_MAX_NACKS 16
#define PROTO_MAX_INPUT_EVENTS 32

/* ---- control channel framing: u32 length, u16 type, u16 flags, payload */

enum proto_ctl_type {
	PROTO_CTL_HELLO = 0x0001,
	PROTO_CTL_SETUP = 0x0002,
	PROTO_CTL_REJECT = 0x0003,
	PROTO_CTL_READY = 0x0004,
	PROTO_CTL_RECONFIGURE = 0x0010,
	PROTO_CTL_RECONFIGURE_ACK = 0x0011,
	PROTO_CTL_DISPLAY_CHANGED = 0x0012,
	PROTO_CTL_CURSOR_SHAPE = 0x0020,
	PROTO_CTL_PING = 0x0030,
	PROTO_CTL_PONG = 0x0031,
	PROTO_CTL_CLOSE = 0x00ff,
};

/* Writes one framed message. Returns the total bytes written, or a negative errno. */
int proto_ctl_frame_write(uint16_t type, const uint8_t *payload, size_t len, uint8_t *out, size_t cap);

/* Parses the first frame in a byte stream. Returns the bytes consumed (frame header plus payload), 0 if more data is
 * needed, or a negative errno if the frame is invalid (length below 4 or above the limit). On success `*payload`
 * points into `buf` and stays valid as long as `buf` does. */
int proto_ctl_frame_parse(const uint8_t *buf, size_t len, uint16_t *type, const uint8_t **payload, size_t *payload_len);

/* ---- control messages */

enum { PROTO_CAP_TOUCH = 1, PROTO_CAP_STYLUS = 2, PROTO_CAP_HDR10 = 4, PROTO_CAP_WIDE_GAMUT = 8, PROTO_CAP_HLG = 16 };
enum { PROTO_CODEC_HEVC = 1, PROTO_CODEC_AV1 = 2, PROTO_CODEC_HEVC_MAIN10 = 3 };
enum { PROTO_SECURITY_NONE = 0, PROTO_SECURITY_CHACHA20POLY1305 = 1 };

struct proto_hello {
	uint16_t proto_min, proto_max;
	char device_name[PROTO_NAME_MAX];
	uint32_t device_caps;
	uint16_t native_width, native_height, density_dpi;
	uint8_t orientation; /* 0..3 */
	uint16_t inset_left, inset_top, inset_right, inset_bottom;
	uint32_t refresh_mhz[PROTO_MAX_RATES];
	uint8_t n_refresh;
	uint8_t codecs[PROTO_MAX_CODECS];
	uint8_t n_codecs;
	uint8_t max_touch_points;
	uint16_t max_udp_payload;
	char app_version[PROTO_VERSION_STR_MAX];
};

/* The part of SETUP that RECONFIGURE repeats. */
struct proto_stream_params {
	uint8_t epoch;
	uint8_t security;
	uint16_t width, height;
	uint32_t refresh_mhz;
	uint8_t codec;
	uint8_t color_range, color_primaries, color_transfer, color_matrix; /* H.273 */
	uint16_t shard_size;
	uint8_t fec_percent;
	uint32_t bitrate_kbps, min_bitrate_kbps, max_bitrate_kbps;
	uint8_t dpb_frames;
	uint16_t param_sets_len;
	uint8_t param_sets[PROTO_MAX_PARAM_SETS]; /* VPS, SPS, PPS in Annex B */
};

struct proto_setup {
	uint16_t proto_version;
	uint32_t session_id;
	uint16_t udp_port;
	struct proto_stream_params params;
};

struct proto_reject {
	uint16_t reason; /* 1 no common version, 2 not authorised, 3 busy, 4 unsupported display */
	char text[PROTO_TEXT_MAX];
};

struct proto_cursor_shape {
	uint16_t shape_id;
	int16_t hot_x, hot_y;
	uint16_t width, height;
	const uint8_t *argb; /* width * height * 4 bytes; points into the decoded buffer */
};

struct proto_close {
	uint16_t reason; /* 0 normal, 1 screen lock, 2 replaced by another phone, 3 error */
	char text[PROTO_TEXT_MAX];
};

int proto_hello_encode(const struct proto_hello *m, uint8_t *out, size_t cap);
int proto_hello_decode(const uint8_t *buf, size_t len, struct proto_hello *m);
int proto_setup_encode(const struct proto_setup *m, uint8_t *out, size_t cap);
int proto_setup_decode(const uint8_t *buf, size_t len, struct proto_setup *m);
int proto_stream_params_encode(const struct proto_stream_params *m, uint8_t *out, size_t cap); /* RECONFIGURE */
int proto_stream_params_decode(const uint8_t *buf, size_t len, struct proto_stream_params *m);
int proto_reject_encode(const struct proto_reject *m, uint8_t *out, size_t cap);
int proto_reject_decode(const uint8_t *buf, size_t len, struct proto_reject *m);
int proto_cursor_shape_encode(const struct proto_cursor_shape *m, uint8_t *out, size_t cap);
int proto_cursor_shape_decode(const uint8_t *buf, size_t len, struct proto_cursor_shape *m);
int proto_close_encode(const struct proto_close *m, uint8_t *out, size_t cap);
int proto_close_decode(const uint8_t *buf, size_t len, struct proto_close *m);
/* PING and PONG carry one u64 token */
int proto_token_encode(uint64_t token, uint8_t *out, size_t cap);
int proto_token_decode(const uint8_t *buf, size_t len, uint64_t *token);
/* DISPLAY_CHANGED repeats the display fields of HELLO; it uses the HELLO codec. */

/* ---- UDP packets: 16-byte common header, then the payload (the AEAD tag, when used, follows and is not handled here) */

enum proto_pkt_type {
	PROTO_PKT_HELLO_UDP = 0x00,
	PROTO_PKT_VIDEO = 0x01,
	PROTO_PKT_CURSOR_POS = 0x02,
	PROTO_PKT_KEEPALIVE = 0x03,
	PROTO_PKT_FEEDBACK = 0x10,
	PROTO_PKT_INPUT = 0x11,
	PROTO_PKT_INPUT_ACK = 0x12,
	PROTO_PKT_TIME_REQ = 0x14,
	PROTO_PKT_TIME_RESP = 0x15,
};

#define PROTO_PKT_HDR_SIZE 16
#define PROTO_VIDEO_HDR_SIZE 28
#define PROTO_AEAD_TAG_SIZE 16
#define PROTO_PKT_OVERHEAD (PROTO_PKT_HDR_SIZE + PROTO_VIDEO_HDR_SIZE + PROTO_AEAD_TAG_SIZE) /* 60 */
#define PROTO_NO_REF 0xffffffffu /* ref_frame_id of an IDR */

enum { PROTO_PKT_FLAG_REPAIR = 1 };
enum { PROTO_VFLAG_IDR = 1, PROTO_VFLAG_NEW_EPOCH = 2, PROTO_VFLAG_REFRESH = 4 };

struct proto_pkt_hdr {
	uint32_t session_id;
	uint8_t type;
	uint8_t flags;
	uint64_t seq;
};

struct proto_video_hdr {
	uint32_t frame_id;
	uint32_t ref_frame_id;
	uint64_t capture_us;
	uint32_t frame_bytes;
	uint8_t epoch;
	uint8_t block_index, block_count;
	uint8_t shard_index, data_shards, parity_shards;
	uint8_t vflags;
};

struct proto_keepalive {
	uint32_t newest_frame_id;
	uint64_t now_us;
	uint32_t input_acked;
};

struct proto_nack {
	uint32_t frame_id;
	uint8_t block_index;
	uint8_t need; /* shards still missing to reconstruct the block */
};

enum { PROTO_FB_NEED_IDR = 1 };

struct proto_feedback {
	uint8_t flags;
	uint32_t newest_frame_id;
	uint32_t held_base;   /* newest frame such that every frame up to it is resolved (decoded or given up) */
	uint64_t held_bitmap; /* bit i: frame held_base - i was decoded (set) or given up (clear) */
	uint8_t nack_count;
	struct proto_nack nacks[PROTO_MAX_NACKS];
	uint16_t loss_permille;
	int32_t queue_delay_us; /* median recent one-way delay minus its minimum over the last ~2 s; 0 or more */
	uint32_t recv_kbps;
	uint32_t jitter_us;
	uint16_t decode_ms_x10;
};

enum { PROTO_IN_DOWN = 0, PROTO_IN_MOVE = 1, PROTO_IN_UP = 2, PROTO_IN_CANCEL = 3, PROTO_IN_HOVER = 4, PROTO_IN_BUTTON = 5 };
enum { PROTO_TOOL_FINGER = 0, PROTO_TOOL_STYLUS = 1, PROTO_TOOL_ERASER = 2 };

struct proto_input_event {
	uint32_t seq;
	uint8_t kind, pointer_id, tool, buttons;
	uint16_t x, y, pressure; /* x, y normalised over the phone's display, 0..65535 */
	int8_t tilt_x, tilt_y;
	uint32_t time_us;
};

struct proto_input {
	uint8_t count;
	struct proto_input_event events[PROTO_MAX_INPUT_EVENTS];
};

struct proto_cursor_pos {
	uint16_t shape_id; /* 0 = hidden */
	uint16_t x, y;
	uint32_t move_seq;
};

struct proto_time_resp {
	uint64_t t1, t2, t3;
};

int proto_pkt_hdr_write(const struct proto_pkt_hdr *h, uint8_t *out, size_t cap);
int proto_pkt_hdr_read(const uint8_t *buf, size_t len, struct proto_pkt_hdr *h);

int proto_video_hdr_write(const struct proto_video_hdr *h, uint8_t *out, size_t cap);
int proto_video_hdr_read(const uint8_t *buf, size_t len, struct proto_video_hdr *h);

int proto_keepalive_encode(const struct proto_keepalive *m, uint8_t *out, size_t cap);
int proto_keepalive_decode(const uint8_t *buf, size_t len, struct proto_keepalive *m);
int proto_feedback_encode(const struct proto_feedback *m, uint8_t *out, size_t cap);
int proto_feedback_decode(const uint8_t *buf, size_t len, struct proto_feedback *m);
int proto_input_encode(const struct proto_input *m, uint8_t *out, size_t cap);
int proto_input_decode(const uint8_t *buf, size_t len, struct proto_input *m);
int proto_u32_encode(uint32_t v, uint8_t *out, size_t cap); /* INPUT_ACK payload */
int proto_u32_decode(const uint8_t *buf, size_t len, uint32_t *v);
int proto_cursor_pos_encode(const struct proto_cursor_pos *m, uint8_t *out, size_t cap);
int proto_cursor_pos_decode(const uint8_t *buf, size_t len, struct proto_cursor_pos *m);
int proto_time_req_encode(uint64_t t1, uint8_t *out, size_t cap); /* same layout as a token */
int proto_time_resp_encode(const struct proto_time_resp *m, uint8_t *out, size_t cap);
int proto_time_resp_decode(const uint8_t *buf, size_t len, struct proto_time_resp *m);

#endif
