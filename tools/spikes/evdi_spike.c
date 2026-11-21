// THROWAWAY spike (M0): does evdi give us EDID identity, hotplug and frames?
// Build: gcc -O2 -I<evdi>/library evdi_spike.c -L<evdi>/lib -levdi -o evdi_spike
// Run:   ./evdi_spike <card-index> <seconds> [dump.ppm]
#include <evdi_lib.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static double t0;
#define LOG(...) do { printf("[%7.3f] ", now() - t0); printf(__VA_ARGS__); fflush(stdout); } while (0)

// ---- EDID 1.4 base block: one detailed timing (CVT-RB-ish), name, range, serial
static void put_text(uint8_t *d, uint8_t tag, const char *s)
{
	memset(d, 0, 18);
	d[3] = tag;
	memset(d + 5, 0x20, 13);
	size_t n = strlen(s);
	if (n > 13) n = 13;
	memcpy(d + 5, s, n);
	if (n < 13) d[5 + n] = 0x0A;
}

static int g_hz = 60;   // MODE=WxH@Hz overrides the default 2400x1080@60

static void make_edid(uint8_t e[128], int w, int h, int mm_w, int mm_h,
		      uint32_t serial, const char *name, const char *sn)
{
	memset(e, 0, 128);
	static const uint8_t hdr[8] = { 0, 255, 255, 255, 255, 255, 255, 0 };
	memcpy(e, hdr, 8);
	e[8] = 0x40; e[9] = 0x89;            // PNP "PDI" (unregistered, spike only)
	e[10] = 0x01; e[11] = 0x00;          // product code
	memcpy(e + 12, &serial, 4);
	e[16] = 1; e[17] = 2026 - 1990;
	e[18] = 1; e[19] = 4;
	e[20] = 0xA5;                        // digital, 8bpc, DisplayPort
	e[21] = (mm_w + 5) / 10; e[22] = (mm_h + 5) / 10;
	e[23] = 120;                         // gamma 2.2
	e[24] = 0x06;                        // sRGB, preferred = native
	static const uint8_t chroma[10] = { 0xEE, 0x91, 0xA3, 0x54, 0x4C, 0x99, 0x26, 0x0F, 0x50, 0x54 };
	memcpy(e + 25, chroma, 10);
	for (int i = 0; i < 8; i++) { e[38 + 2 * i] = 1; e[39 + 2 * i] = 1; }

	// detailed timing; reduced blanking: hblank 160 (fp 48, sync 32), vblank 31 (fp 3, sync 10)
	int hb = 160, hfp = 48, hsw = 32, vb = g_hz > 60 ? 60 : 31, vfp = 3, vsw = 10;
	long clk = (long)(w + hb) * (h + vb) * g_hz / 10000;
	uint8_t *d = e + 54;
	d[0] = clk & 0xFF; d[1] = clk >> 8;
	d[2] = w & 0xFF; d[3] = hb & 0xFF; d[4] = ((w >> 8) << 4) | (hb >> 8);
	d[5] = h & 0xFF; d[6] = vb & 0xFF; d[7] = ((h >> 8) << 4) | (vb >> 8);
	d[8] = hfp & 0xFF; d[9] = hsw & 0xFF;
	d[10] = ((vfp & 0xF) << 4) | (vsw & 0xF);
	d[11] = ((hfp >> 8) << 6) | ((hsw >> 8) << 4) | ((vfp >> 4) << 2) | (vsw >> 4);
	d[12] = mm_w & 0xFF; d[13] = mm_h & 0xFF; d[14] = ((mm_w >> 8) << 4) | (mm_h >> 8);
	d[17] = 0x1A;                        // digital separate sync, +hsync -vsync

	put_text(e + 72, 0xFC, name);
	uint8_t *r = e + 90;                 // range limits
	memset(r, 0, 18);
	r[3] = 0xFD; r[5] = 30; r[6] = g_hz; r[7] = 30; r[8] = (int)((long)(h + vb) * g_hz / 1000) + 5; r[9] = clk / 1000 + 1; r[10] = 0x01;
	r[11] = 0x0A; memset(r + 12, 0x20, 6);
	put_text(e + 108, 0xFF, sn);

	uint8_t sum = 0;
	for (int i = 0; i < 127; i++) sum += e[i];
	e[127] = (uint8_t)(256 - sum);
}

// ---- frame handling
static struct evdi_buffer buf;
static int have_buf, frames, last_rects;
static FILE *raw;   // RAW_OUT: write every grabbed frame as raw BGRA (for the encode spike)
static struct evdi_rect rects[16];

static void grab(evdi_handle h)
{
	int n = 16;
	double a = now();
	evdi_grab_pixels(h, rects, &n);
	double b = now();
	frames++;
	last_rects = n;
	if (raw) fwrite(buf.buffer, 1, (size_t)buf.stride * buf.height, raw);
	LOG("frame #%d grab=%.2f ms rects=%d", frames, (b - a) * 1e3, n);
	for (int i = 0; i < n && i < 3; i++)
		printf(" [%d,%d-%d,%d]", rects[i].x1, rects[i].y1, rects[i].x2, rects[i].y2);
	printf("\n");
}

static void on_mode(struct evdi_mode m, void *u)
{
	evdi_handle h = *(evdi_handle *)u;
	LOG("mode_changed %dx%d@%d bpp=%d\n", m.width, m.height, m.refresh_rate, m.bits_per_pixel);
	if (have_buf) { evdi_unregister_buffer(h, buf.id); free(buf.buffer); }
	buf.id = 1; buf.width = m.width; buf.height = m.height; buf.stride = m.width * 4;
	buf.buffer = calloc(1, (size_t)buf.stride * buf.height);
	buf.rects = rects; buf.rect_count = 16;
	evdi_register_buffer(h, buf);
	have_buf = 1;
}
static void on_update(int id, void *u) { grab(*(evdi_handle *)u); }
static void on_dpms(int m, void *u) { LOG("dpms %d\n", m); }
static void on_crtc(int s, void *u) { LOG("crtc_state %d\n", s); }
static void on_cset(struct evdi_cursor_set c, void *u) { LOG("cursor_set %ux%u enabled=%d\n", c.width, c.height, c.enabled); free(c.buffer); }
static void on_cmove(struct evdi_cursor_move c, void *u) { (void)c; }

int main(int argc, char **argv)
{
	if (argc < 3) { fprintf(stderr, "usage: %s card secs [dump.ppm]\n", argv[0]); return 2; }
	int card = atoi(argv[1]); double secs = atof(argv[2]);
	t0 = now();

	struct evdi_lib_version v; evdi_get_lib_version(&v);
	LOG("libevdi %d.%d.%d, device %d status=%d (0=AVAILABLE)\n", v.version_major, v.version_minor, v.version_patchlevel, card, evdi_check_device(card));

	uint8_t edid[128];
	int mw = 2400, mh = 1080, mmw = 148, mmh = 66;
	if (getenv("MODE")) {   // e.g. 2304x1440@90 (physical size scaled to a ~11" tablet)
		sscanf(getenv("MODE"), "%dx%d@%d", &mw, &mh, &g_hz);
		mmw = mw * 234 / 2304; mmh = mh * 150 / 1440;
	}
	make_edid(edid, mw, mh, mmw, mmh, 0x00000001, "pdisp-phone", "PDISP0001");
	FILE *f = getenv("EDID_OUT") ? fopen(getenv("EDID_OUT"), "wb") : NULL;
	if (f) { fwrite(edid, 1, 128, f); fclose(f); }

	if (getenv("RAW_OUT")) raw = fopen(getenv("RAW_OUT"), "wb");
	evdi_handle h = evdi_open(card);
	if (h == EVDI_INVALID_HANDLE) { LOG("evdi_open failed\n"); return 1; }
	struct evdi_event_context ctx = { .dpms_handler = on_dpms, .mode_changed_handler = on_mode,
		.update_ready_handler = on_update, .crtc_state_handler = on_crtc,
		.cursor_set_handler = on_cset, .cursor_move_handler = on_cmove, .user_data = &h };
	evdi_enable_cursor_events(h, true);

	int connected = 0, cycled = 0;
	double cyc_t = 0;
	evdi_connect2(h, edid, 128, 4096 * 2160, 4096 * 2160 * 60);
	connected = 1; LOG("connected with EDID\n");

	struct pollfd pfd = { .fd = evdi_get_event_ready(h), .events = POLLIN };
	while (now() - t0 < secs) {
		if (!getenv("NO_CYCLE") && !cycled && now() - t0 > secs * 0.6) {
			LOG("disconnect\n"); evdi_disconnect(h); connected = 0; cycled = 1; cyc_t = now();
		}
		if (!connected && now() - cyc_t > 3.0) {
			LOG("reconnect (same EDID)\n");
			evdi_connect2(h, edid, 128, 4096 * 2160, 4096 * 2160 * 60); connected = 1;
		}
		// a frame that is already ready is grabbed synchronously; only wait for an event when none is
		if (connected && have_buf && evdi_request_update(h, buf.id)) { grab(h); if (poll(&pfd, 1, 0) > 0) evdi_handle_events(h, &ctx); continue; }
		if (poll(&pfd, 1, 100) > 0) evdi_handle_events(h, &ctx);
	}
	LOG("done: %d frames total\n", frames);
	if (argc > 3 && have_buf) {
		FILE *p = fopen(argv[3], "wb");
		fprintf(p, "P6\n%d %d\n255\n", buf.width, buf.height);
		uint8_t *s = buf.buffer;
		for (int i = 0; i < buf.width * buf.height; i++) { fputc(s[i*4+2], p); fputc(s[i*4+1], p); fputc(s[i*4], p); }
		fclose(p); LOG("wrote %s\n", argv[3]);
	}
	if (raw) fclose(raw);
	evdi_disconnect(h);
	evdi_close(h);
	return 0;
}
