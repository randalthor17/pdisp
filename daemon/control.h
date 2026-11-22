/* The control socket of pdispd (docs/control.md): a Unix stream socket speaking one JSON object per line. The daemon publishes its state, a GUI or
 * a script reads it, answers pairing requests and changes settings. The server runs in its own thread: the streaming loop only copies a snapshot
 * into it (control_publish) and never waits for a client. */
#ifndef PDISP_CONTROL_H
#define PDISP_CONTROL_H

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>

#include "json.h"

/* What the daemon tells its clients. Plain values, copied under a lock. */
struct ctl_state {
	bool usb, net;                 /* transports in use */
	unsigned port;
	char usb_state[12];            /* "off", "waiting" (looking for a cable) or "accessory" (a device is in accessory mode) */
	bool session;                  /* a device is connected */
	char device[64];
	char transport[8];             /* "usb" or "network" */
	unsigned width, height;
	double refresh_hz, session_secs;
	double fps, bitrate_mbps, queue_ms, phone_queue_ms, phone_decode_ms;
	unsigned long long frames_lost, idr_frames, repair_packets;
	bool monitor;                  /* the virtual monitor exists (also during the grace period) */
	char connector[32];
	bool grace;                    /* the monitor is kept for a device that was lost */
	/* settings, as they are now (the ones a client may change: see set_config) */
	unsigned refresh_cap_hz, bitrate_kbps, max_bitrate_kbps, grace_secs, refine_frames;
};

struct control_ops {
	void (*disconnect)(void *user);                       /* end the session now, remove the monitor */
	int (*devices)(void *user, struct jw *w);             /* append "devices":[{fingerprint,name}] to the reply */
	int (*remove_device)(void *user, const char *prefix); /* number removed, or a negative errno */
	/* change one setting and keep it for the next start; 0 or a negative errno with the reason in err */
	int (*set_config)(void *user, const char *key, const char *value, char *err, size_t errcap);
	void *user;
};

/* Listens on `path` (created with mode 0600, in a 0700 directory). Returns 0, or a negative errno. */
int control_start(const char *path, const struct control_ops *ops);
void control_stop(void);
void control_publish(const struct ctl_state *s);

/* Asks the connected GUI to approve a device. Returns -2 if no client is listening for pairing requests (the caller falls back to its own prompt),
 * otherwise 'y' (always), 'o' (this time only), 'n' (deny), or -1 on timeout or when *stop is set. */
int control_pair_ask(const char *name, const char *fingerprint, unsigned code, int timeout_ms, const volatile sig_atomic_t *stop);

#endif
