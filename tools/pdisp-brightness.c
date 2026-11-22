/* pdisp brightness: read or set the phone's brightness over DDC/CI (docs/brightness.md), the way a shell's slider does, without the monitor
 * probing `ddcutil` does first (and fails on).
 *   pdisp brightness [-d /dev/i2c-N]            print it
 *   pdisp brightness [-d /dev/i2c-N] 40         set it (0..100)
 *   pdisp brightness [-d /dev/i2c-N] +10 | -10  change it (for keybindings; write it as `-- -10` after any option)
 * Without -d, the I2C buses of the evdi adapters are tried until one answers (the one with a phone connected). */
#include <dirent.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "pdisp-cli.h"

static int send_frame(int fd, const uint8_t *payload, size_t n)
{
	uint8_t b[16] = { 0x51, (uint8_t)(0x80 | n) };
	memcpy(b + 2, payload, n);
	uint8_t c = 0x6e;
	for (size_t i = 0; i < 2 + n; i++) c ^= b[i];
	b[2 + n] = c;
	return write(fd, b, 3 + n) == (ssize_t)(3 + n) ? 0 : -1;
}

/* Reads brightness and its maximum from the monitor on `dev`. Returns 0, or -1 if nothing answered properly. */
static int get(const char *dev, unsigned *cur, unsigned *max, int *fd_out)
{
	int fd = open(dev, O_RDWR | O_CLOEXEC);
	if (fd < 0) return -1;
	if (ioctl(fd, I2C_SLAVE_FORCE, 0x37) < 0 || send_frame(fd, (uint8_t[]){ 0x01, 0x10 }, 2)) { close(fd); return -1; }
	nanosleep(&(struct timespec){ 0, 50000000 }, NULL); /* the specification asks the host to wait */
	uint8_t r[11];
	if (read(fd, r, sizeof r) != (ssize_t)sizeof r) { close(fd); return -1; }
	uint8_t c = 0x50;
	for (int i = 0; i < 10; i++) c ^= r[i];
	if (r[0] != 0x6e || r[1] != 0x88 || r[2] != 0x02 || c != r[10] || r[3]) { close(fd); return -1; }
	*cur = (unsigned)(r[8] << 8 | r[9]);
	*max = (unsigned)(r[6] << 8 | r[7]);
	*fd_out = fd;
	return 0;
}

/* the adapter names evdi gives its buses: look them up in sysfs and offer each as /dev/i2c-N */
static int find_devices(char out[][320], int cap)
{
	int n = 0;
	DIR *d = opendir("/sys/class/i2c-dev");
	struct dirent *e;
	while (d && (e = readdir(d)) && n < cap) {
		if (strncmp(e->d_name, "i2c-", 4)) continue;
		char p[300], name[64] = "";
		snprintf(p, sizeof p, "/sys/class/i2c-dev/%s/name", e->d_name);
		FILE *f = fopen(p, "r");
		if (f) { if (fgets(name, sizeof name, f)) {} fclose(f); }
		if (strncmp(name, "DisplayLink I2C Adapter", 23)) continue;
		snprintf(out[n++], 32, "/dev/%.20s", e->d_name);
	}
	if (d) closedir(d);
	return n;
}

int brightness_main(int argc, char **argv)
{
	const char *dev = NULL, *arg = NULL;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-d") && i + 1 < argc) dev = argv[++i];
		else if (!strcmp(argv[i], "--")) { if (i + 1 < argc) arg = argv[++i]; }
		else if (!arg) arg = argv[i];
		else { fprintf(stderr, "usage: pdisp brightness [-d /dev/i2c-N] [N | +N | -N]\n"); return 2; }
	}
	char found[8][320];
	int n = 0;
	if (dev) snprintf(found[n++], sizeof found[0], "%.300s", dev);
	else n = find_devices(found, 8);
	if (!n) { fprintf(stderr, "no evdi I2C bus found (no phone is connected, or the evdi module is not loaded)\n"); return 1; }

	unsigned cur = 0, max = 100;
	int fd = -1;
	for (int i = 0; i < n && fd < 0; i++) if (get(found[i], &cur, &max, &fd) == 0) dev = found[i];
	if (fd < 0) { fprintf(stderr, "no phone answered on %s (is one connected, and may you use the bus? the group i2c)\n", n == 1 ? found[0] : "the evdi buses"); return 1; }
	if (!arg) { printf("brightness %u of %u\n", cur, max); close(fd); return 0; }

	char *end;
	long v = strtol(arg, &end, 10);
	if (*end || end == arg) { fprintf(stderr, "a number, +N or -N\n"); close(fd); return 2; }
	if (arg[0] == '+' || arg[0] == '-') v += (long)cur;
	if (v < 0) v = 0;
	if (v > (long)max) v = (long)max;
	int rc = send_frame(fd, (uint8_t[]){ 0x03, 0x10, (uint8_t)(v >> 8), (uint8_t)v }, 4) ? 1 : 0;
	if (rc) perror("write");
	else printf("brightness %ld of %u\n", v, max);
	close(fd);
	return rc;
}
