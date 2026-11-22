/* A minimal DDC/CI monitor (VESA DDC/CI, address 0x37) for the virtual display, so that `ddcutil`, a desktop shell's brightness slider
 * and anything else that talks DDC/CI to a monitor can set the phone's brightness. The kernel's evdi I2C adapter passes every
 * transaction on 0x37 to userspace; this module is the pure part: bytes in, bytes out.
 *
 * It answers Get VCP Feature, Set VCP Feature and Capabilities Request for one feature, brightness (VCP 0x10, 0..100). A write
 * is a request frame from the host; the reply to it is held until the host reads. */
#ifndef PDISP_DDCCI_H
#define PDISP_DDCCI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DDCCI_MAX_FRAME 64 /* what evdi passes through at most */

struct ddcci {
	unsigned brightness; /* 0..100 */
	uint8_t reply[DDCCI_MAX_FRAME];
	size_t reply_len; /* 0: nothing to read (a read then gives the null message) */
};

void ddcci_init(struct ddcci *d, unsigned brightness);

/* A write from the host (the frame starts with the source address 0x51). Returns true if it set the brightness (the new value is
 * then in d->brightness). A malformed frame is ignored. */
bool ddcci_write(struct ddcci *d, const uint8_t *buf, size_t len);

/* A read by the host: fills buf[0..len) with the pending reply (zero padded), or the null message if there is none. */
void ddcci_read(struct ddcci *d, uint8_t *buf, size_t len);

#endif
