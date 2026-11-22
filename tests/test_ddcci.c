/* The DDC/CI monitor: frames as ddcutil sends them, checksums, replies. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../daemon/ddcci.h"

/* a host request frame: 51, 0x80|n, payload, checksum (starting from 6E) */
static size_t req(uint8_t *b, const uint8_t *payload, size_t n)
{
	b[0] = 0x51; b[1] = (uint8_t)(0x80 | n); memcpy(b + 2, payload, n);
	uint8_t c = 0x6e;
	for (size_t i = 0; i < 2 + n; i++) c ^= b[i];
	b[2 + n] = c;
	return 3 + n;
}

static void check_reply(const uint8_t *r)
{
	assert(r[0] == 0x6e && (r[1] & 0x80));
	size_t n = r[1] & 0x7f;
	uint8_t c = 0x50;
	for (size_t i = 0; i < 2 + n; i++) c ^= r[i];
	assert(c == r[2 + n]);
}

int main(void)
{
	struct ddcci d; uint8_t b[64], r[64];
	ddcci_init(&d, 100);

	/* Get VCP 0x10 */
	size_t n = req(b, (const uint8_t[]){ 0x01, 0x10 }, 2);
	assert(!ddcci_write(&d, b, n));
	ddcci_read(&d, r, 11);
	check_reply(r);
	assert(r[1] == 0x88 && r[2] == 0x02 && r[3] == 0 && r[4] == 0x10 && r[6] == 0 && r[7] == 100 && r[8] == 0 && r[9] == 100);

	/* Set VCP 0x10 to 40: it reports the change, and the next Get returns it */
	n = req(b, (const uint8_t[]){ 0x03, 0x10, 0, 40 }, 4);
	assert(ddcci_write(&d, b, n) && d.brightness == 40);
	n = req(b, (const uint8_t[]){ 0x01, 0x10 }, 2);
	ddcci_write(&d, b, n); ddcci_read(&d, r, 11);
	assert(r[9] == 40);

	/* above the maximum clamps */
	n = req(b, (const uint8_t[]){ 0x03, 0x10, 0, 250 }, 4);
	assert(ddcci_write(&d, b, n) && d.brightness == 100);

	/* an unsupported feature: result code 1 */
	n = req(b, (const uint8_t[]){ 0x01, 0x12 }, 2);
	ddcci_write(&d, b, n); ddcci_read(&d, r, 11);
	check_reply(r);
	assert(r[3] == 1);

	/* a set of another feature changes nothing */
	d.brightness = 55;
	n = req(b, (const uint8_t[]){ 0x03, 0x12, 0, 9 }, 4);
	assert(!ddcci_write(&d, b, n) && d.brightness == 55);

	/* a bad checksum is ignored */
	n = req(b, (const uint8_t[]){ 0x03, 0x10, 0, 10 }, 4);
	b[n - 1] ^= 1;
	assert(!ddcci_write(&d, b, n) && d.brightness == 55);

	/* capabilities: the fragments put together are the string, ending with an empty one */
	char caps[256] = ""; size_t off = 0;
	for (int i = 0; i < 20; i++) {
		n = req(b, (const uint8_t[]){ 0xf3, (uint8_t)(off >> 8), (uint8_t)off }, 3);
		ddcci_write(&d, b, n); ddcci_read(&d, r, 40);
		check_reply(r);
		size_t k = (r[1] & 0x7f) - 3;
		assert(r[2] == 0xe3 && r[3] == (off >> 8) && r[4] == (off & 255));
		if (k == 0) break;
		memcpy(caps + off, r + 5, k); off += k;
	}
	assert(strstr(caps, "vcp(10)") && strstr(caps, "mccs_ver(2.2)") && caps[0] == '(' && caps[off - 1] == ')');

	/* a read with nothing pending: the null message; a reply is given once */
	ddcci_read(&d, r, 8);
	assert(r[0] == 0x6e && r[1] == 0x80 && r[2] == (0x6e ^ 0x80 ^ 0x50));

	/* short or odd frames are ignored */
	assert(!ddcci_write(&d, b, 0) && !ddcci_write(&d, b, 2));
	b[0] = 0x50; assert(!ddcci_write(&d, b, 5));

	puts("ok");
	return 0;
}
