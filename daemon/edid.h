/* EDID 1.4 base-block generator for the virtual output.
 *
 * One detailed timing (CVT reduced blanking v1, computed from width/height/refresh), the monitor name,
 * the serial string and the range limits. Compositors identify the output by make/model/serial, so those
 * must be stable per paired device. Physical size is written to the base-block centimetre fields (which
 * niri reads) and to the detailed timing in millimetres.
 */
#ifndef PDISP_EDID_H
#define PDISP_EDID_H

#include <stdint.h>

#define EDID_SIZE 128
#define EDID_TEXT_MAX 13 /* name and serial text descriptors hold 13 characters */

struct edid_params {
	unsigned width;        /* active pixels, 64..4095 */
	unsigned height;       /* active lines, 64..4095 */
	unsigned refresh_mhz;  /* e.g. 60000, 90000, 119880 */
	unsigned mm_width;     /* physical size in mm, 10..2550 */
	unsigned mm_height;
	char pnp_id[4];        /* 3 uppercase letters (unregistered IDs are fine for a private product) */
	uint16_t product_code;
	uint32_t serial;       /* binary serial, little-endian in the header */
	char name[EDID_TEXT_MAX + 1];        /* printable ASCII, 1..13 characters */
	char serial_text[EDID_TEXT_MAX + 1]; /* printable ASCII, 1..13 characters */
	unsigned year;         /* year of manufacture, 1990..2245 */
};

/* The timing chosen for a mode, exposed so tests and the daemon can log it. */
struct edid_timing {
	unsigned hactive, hblank, hfront, hsync;
	unsigned vactive, vblank, vfront, vsync;
	unsigned pixel_clock_khz;
	double refresh_hz; /* actual, after pixel clock rounding */
};

/* Compute the CVT-RB timing. Returns 0, or -EINVAL if the mode cannot be encoded in one detailed timing. */
int edid_compute_timing(unsigned width, unsigned height, unsigned refresh_mhz, struct edid_timing *out);

/* Fill a 128-byte EDID. Returns 0, or -EINVAL on a parameter that cannot be encoded (nothing is written). */
int edid_generate(const struct edid_params *p, uint8_t out[EDID_SIZE]);

#endif
