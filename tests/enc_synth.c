/* Test helper: drive the encoder with a synthetic image (optionally scrolling) and simulate lost frames.
 * usage: enc_synth [-I refresh_frames] [-L slices] [-n frames] [-m] [-X first:last] [-c cbr|vbr] [-o out.hevc]
 *   -m          scroll the pattern 6 px per frame so every block changes and lost frames matter
 *   -X a:b      after frame b, tell the encoder that frames a..b never reached the receiver
 * Prints each frame size; the frame after the loss is followed by "[ref -N]" (N frames back; 0 = IDR). */
#include "encoder.h"
#include "pattern.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	struct encoder_config cfg = { .width = 1920, .height = 1088, .fps = 60, .bitrate_kbps = 20000, .vbv_ms = 30,
				      .vbr = true, .target_percent = 50 };
	int frames = 60, opt;
	bool motion = false; /* -m: scroll the pattern 6 px per frame so every block changes */
	const char *outfile = NULL;
	int lost_a = -1, lost_b = -1;
	double pace_fps = 0;
	while ((opt = getopt(argc, argv, "I:L:n:o:c:mX:W:H:p:")) != -1) {
		switch (opt) {
		case 'I': cfg.intra_refresh_frames = (unsigned)atoi(optarg); break;
		case 'L': cfg.slices = (unsigned)atoi(optarg); break;
		case 'n': frames = atoi(optarg); break;
		case 'o': outfile = optarg; break;
		case 'c': cfg.vbr = strcmp(optarg, "cbr") != 0; break;
		case 'm': motion = true; break;
		case 'W': cfg.width = (unsigned)atoi(optarg); break;
		case 'H': cfg.height = (unsigned)atoi(optarg); break;
		case 'p': pace_fps = atof(optarg); break; /* pace frames like a 60 Hz source and print timing statistics */
		case 'X': sscanf(optarg, "%d:%d", &lost_a, &lost_b); break; /* tell the encoder frames a..b were lost */
		}
	}
	uint8_t *img = malloc((size_t)cfg.width * cfg.height * 4);
	/* -m: scroll the pattern by 6 px per frame so every block changes and lost frames corrupt later ones */
	struct encoder *enc;
	if (encoder_open(&cfg, &enc)) { fprintf(stderr, "encoder_open failed\n"); return 1; }
	FILE *f = outfile ? fopen(outfile, "wb") : NULL;
	size_t total = 0;
	double *tm[3] = { NULL, NULL, NULL }; /* upload, convert, encode per frame */
	if (pace_fps > 0)
		for (int k = 0; k < 3; k++) tm[k] = malloc(sizeof(double) * (size_t)frames);
	struct timespec t_start;
	clock_gettime(CLOCK_MONOTONIC, &t_start);
	printf("static image, %ux%u, refresh=%u slices=%u %s\nsizes:", cfg.width, cfg.height, cfg.intra_refresh_frames,
	       cfg.slices, cfg.vbr ? "vbr" : "cbr");
	for (int i = 0; i < frames; i++) {
		if (i == 0 || motion) {
			unsigned shift = motion ? (unsigned)(6 * i) % cfg.width : 0;
			for (unsigned y = 0; y < cfg.height; y++)
				for (unsigned x = 0; x < cfg.width; x++) {
					uint32_t px = pattern_pixel((x + shift) % cfg.width, y, cfg.width, cfg.height);
					uint8_t *d = img + ((size_t)y * cfg.width + x) * 4;
					d[0] = px & 0xff; d[1] = (px >> 8) & 0xff; d[2] = (px >> 16) & 0xff; d[3] = 0xff;
				}
		}
		if (pace_fps > 0) { /* wait for this frame's slot on a fixed clock */
			struct timespec now;
			clock_gettime(CLOCK_MONOTONIC, &now);
			double due = i / pace_fps, have = (now.tv_sec - t_start.tv_sec) + (now.tv_nsec - t_start.tv_nsec) / 1e9;
			if (due > have) {
				struct timespec nap = { .tv_sec = (time_t)(due - have), .tv_nsec = (long)(((due - have) - (time_t)(due - have)) * 1e9) };
				nanosleep(&nap, NULL);
			}
		}
		struct encoder_packet p;
		if (encoder_encode(enc, img, cfg.width * 4, false, &p)) { fprintf(stderr, "encode failed at %d\n", i); return 1; }
		if (tm[0]) { tm[0][i] = p.upload_ms; tm[1][i] = p.convert_ms; tm[2][i] = p.encode_ms; }
		if (f) fwrite(p.data, 1, p.size, f);
		total += p.size;
		printf(" %zu", p.size);
		if (i == lost_b) encoder_invalidate_from(enc, (uint64_t)lost_a);
		if (i == lost_b + 1) printf("[ref -%d]", p.ref_distance);
	}
	printf("\ntotal %zu bytes over %d frames\n", total, frames);
	if (tm[0]) {
		static const char *names[3] = { "upload ", "convert", "encode " };
		int over = 0;
		for (int i = 1; i < frames; i++) /* frame 0 includes one-time setup */
			if (tm[0][i] + tm[1][i] + tm[2][i] > 1000.0 / pace_fps) over++;
		for (int k = 0; k < 3; k++) {
			double *v = tm[k] + 1; /* skip the first frame */
			int n = frames - 1;
			double sum = 0, mx = 0;
			for (int i = 0; i < n; i++) { sum += v[i]; if (v[i] > mx) mx = v[i]; }
			for (int i = 1; i < n; i++) { double x = v[i]; int j = i - 1; while (j >= 0 && v[j] > x) { v[j + 1] = v[j]; j--; } v[j + 1] = x; }
			printf("TIMING %s ms: avg %6.2f  p50 %6.2f  p99 %6.2f  max %7.2f\n", names[k], sum / n, v[n / 2], v[(int)(n * 0.99)], mx);
		}
		printf("TIMING frames over one frame period (%.1f ms): %d of %d\n", 1000.0 / pace_fps, over, frames - 1);
	}
	if (f) fclose(f);
	encoder_close(enc);
	return 0;
}
