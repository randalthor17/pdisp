/* A bare DDC/CI client for the virtual monitor's I2C bus (docs/brightness.md): reads or sets the brightness (VCP 0x10) the way a shell's slider
 * does, without the monitor probing `ddcutil` does first. usage: pdisp-ddc /dev/i2c-N get | set <0..100> */
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

static int send_frame(int fd, const uint8_t *payload, size_t n)
{
	uint8_t b[16] = { 0x51, (uint8_t)(0x80 | n) };
	memcpy(b + 2, payload, n);
	uint8_t c = 0x6e;
	for (size_t i = 0; i < 2 + n; i++) c ^= b[i];
	b[2 + n] = c;
	return write(fd, b, 3 + n) == (ssize_t)(3 + n) ? 0 : -1;
}

int main(int argc, char **argv)
{
	if (argc < 3) { fprintf(stderr, "usage: %s /dev/i2c-N get | set <0..100>\n", argv[0]); return 2; }
	int fd = open(argv[1], O_RDWR);
	if (fd < 0) { perror(argv[1]); return 1; }
	if (ioctl(fd, I2C_SLAVE_FORCE, 0x37) < 0) { perror("I2C_SLAVE"); return 1; }
	if (!strcmp(argv[2], "set") && argc == 4) {
		unsigned v = (unsigned)atoi(argv[3]);
		if (send_frame(fd, (uint8_t[]){ 0x03, 0x10, (uint8_t)(v >> 8), (uint8_t)v }, 4)) { perror("write"); return 1; }
		return 0;
	}
	if (strcmp(argv[2], "get")) { fprintf(stderr, "get or set <value>\n"); return 2; }
	if (send_frame(fd, (uint8_t[]){ 0x01, 0x10 }, 2)) { perror("write"); return 1; }
	nanosleep(&(struct timespec){ 0, 50000000 }, NULL); /* the specification asks the host to wait */
	uint8_t r[11];
	if (read(fd, r, sizeof r) != (ssize_t)sizeof r) { perror("read"); return 1; }
	uint8_t c = 0x50;
	for (int i = 0; i < 10; i++) c ^= r[i];
	if (r[0] != 0x6e || r[1] != 0x88 || r[2] != 0x02 || c != r[10]) { fprintf(stderr, "unexpected reply\n"); return 1; }
	if (r[3]) { fprintf(stderr, "the monitor does not support brightness\n"); return 1; }
	printf("brightness %u of %u\n", r[8] << 8 | r[9], r[6] << 8 | r[7]);
	return 0;
}
