#include "ddcci.h"

#include <string.h>

#define HOST_WRITE_ADDR 0x6e /* the destination address of a host write, 8-bit form: it starts the request checksum */
#define MON_SRC 0x6e         /* a reply starts with this */
#define VIRTUAL_HOST 0x50    /* ... and its checksum starts with this */
#define VCP_BRIGHTNESS 0x10

static const char CAPS[] = "(prot(monitor)type(lcd)model(pdisp)cmds(01 02 03 0C E3 F3)vcp(10)mccs_ver(2.2))";

static uint8_t xor_all(uint8_t seed, const uint8_t *p, size_t n)
{
	while (n--) seed ^= *p++;
	return seed;
}

/* the reply frame for `payload`: 6E, 0x80 | length, payload, checksum */
static void set_reply(struct ddcci *d, const uint8_t *payload, size_t n)
{
	d->reply[0] = MON_SRC;
	d->reply[1] = (uint8_t)(0x80 | n);
	memcpy(d->reply + 2, payload, n);
	d->reply[2 + n] = xor_all(VIRTUAL_HOST, d->reply, 2 + n);
	d->reply_len = 3 + n;
}

void ddcci_init(struct ddcci *d, unsigned brightness)
{
	memset(d, 0, sizeof *d);
	d->brightness = brightness > 100 ? 100 : brightness;
}

bool ddcci_write(struct ddcci *d, const uint8_t *buf, size_t len)
{
	d->reply_len = 0;
	if (len < 4 || buf[0] != 0x51 || (buf[1] & 0x80) == 0) return false;
	size_t n = buf[1] & 0x7f; /* payload length */
	if (n == 0 || 3 + n > len) return false;
	if (xor_all(HOST_WRITE_ADDR, buf, 2 + n) != buf[2 + n]) return false; /* bad checksum: a real monitor ignores it too */
	const uint8_t *p = buf + 2;

	switch (p[0]) {
	case 0x01: /* Get VCP Feature */
		if (n != 2) return false;
		{
			uint8_t r[8] = { 0x02, 0, p[1], 1, 0, 0, 0, 0 };
			if (p[1] == VCP_BRIGHTNESS) { r[4] = 0; r[5] = 100; r[6] = 0; r[7] = (uint8_t)d->brightness; }
			else r[1] = 1; /* unsupported feature */
			set_reply(d, r, 8);
		}
		return false;
	case 0x03: /* Set VCP Feature */
		if (n != 4 || p[1] != VCP_BRIGHTNESS) return false;
		{
			unsigned v = (unsigned)(p[2] << 8 | p[3]);
			d->brightness = v > 100 ? 100 : v;
		}
		return true;
	case 0xf3: { /* Capabilities Request: a fragment of the string from the given offset */
		if (n != 3) return false;
		size_t off = (size_t)(p[1] << 8 | p[2]), total = sizeof CAPS - 1, k = off < total ? total - off : 0;
		if (k > 32) k = 32;
		uint8_t r[3 + 32] = { 0xe3, p[1], p[2] };
		memcpy(r + 3, CAPS + off, k);
		set_reply(d, r, 3 + k);
		return false;
	}
	default: /* Save Current Settings (0x0c) and the rest: no reply */
		return false;
	}
}

void ddcci_read(struct ddcci *d, uint8_t *buf, size_t len)
{
	memset(buf, 0, len);
	if (d->reply_len == 0) { /* the null message: no data */
		static const uint8_t nul[3] = { MON_SRC, 0x80, MON_SRC ^ 0x80 ^ VIRTUAL_HOST };
		memcpy(buf, nul, len < 3 ? len : 3);
		return;
	}
	memcpy(buf, d->reply, len < d->reply_len ? len : d->reply_len);
	d->reply_len = 0;
}
