#include "usblink.h"

#include <errno.h>
#include <libusb.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define AOA_GET_PROTOCOL 51
#define AOA_SEND_STRING 52
#define AOA_START 53
#define GOOGLE_VID 0x18d1
#define CHUNK 16384

struct usblink {
	libusb_context *ctx;
	libusb_device_handle *h;
	int iface;
	unsigned char ep_in, ep_out;
	int max_out; /* wMaxPacketSize of the OUT endpoint */
	int in_pipe[2];  /* bulk IN -> in_pipe[1]; the application reads in_pipe[0] */
	int out_pipe[2]; /* the application writes out_pipe[1]; -> bulk OUT */
	pthread_t t_in, t_out;
	int started;
	atomic_int stop, gone;
	char desc[64];
};

#define DBG(...) do { if (getenv("PDISP_USB_DEBUG")) { fprintf(stderr, "usblink: " __VA_ARGS__); fputc('\n', stderr); } } while (0)

/* When the last switch to accessory mode was requested (microseconds, monotonic). usblink_open() is called again and again while the daemon
 * waits for a phone, in short slices; a device needs about a second to re-enumerate after the request, and asking again in that time
 * restarts the switch, so a recent request is waited for instead of repeated. */
static uint64_t last_switch_us;
static int ignored_switches; /* requests since an accessory last appeared */
static int warned_switch; /* said once that a request was ignored; reset when an accessory shows up */

static uint64_t mono_us(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000ull + (uint64_t)t.tv_nsec / 1000;
}

static void msleep(int ms)
{
	struct timespec t = { ms / 1000, (ms % 1000) * 1000000L };
	nanosleep(&t, NULL);
}

static int is_accessory(const struct libusb_device_descriptor *d)
{
	return d->idVendor == GOOGLE_VID && d->idProduct >= 0x2d00 && d->idProduct <= 0x2d05;
}

static int send_str(libusb_device_handle *h, int idx, const char *s)
{
	return libusb_control_transfer(h, LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_ENDPOINT_OUT, AOA_SEND_STRING, 0, (uint16_t)idx,
				       (unsigned char *)s, (uint16_t)(strlen(s) + 1), 1000);
}

/* Asks one device to switch to accessory mode. Returns 0 if the request was accepted. */
static int switch_to_accessory(libusb_device_handle *h)
{
	unsigned char ver[2] = { 0, 0 };
	int r = libusb_control_transfer(h, LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_ENDPOINT_IN, AOA_GET_PROTOCOL, 0, 0, ver, 2, 1000);
	if (r != 2 || (ver[0] | ver[1] << 8) < 1) return -ENOTSUP;
	/* the app's accessory filter (android/app/src/main/res/xml/accessory_filter.xml) matches manufacturer and model */
	if (send_str(h, 0, "pdisp") < 0 || send_str(h, 1, "pdisp") < 0 || send_str(h, 2, "pdisp second monitor") < 0 ||
	    send_str(h, 3, "1") < 0 || send_str(h, 4, "") < 0 || send_str(h, 5, "0") < 0)
		return -EIO;
	r = libusb_control_transfer(h, LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_ENDPOINT_OUT, AOA_START, 0, 0, NULL, 0, 1000);
	return r < 0 ? -EIO : 0;
}

static int path_of(libusb_device *dev, uint8_t *bus, uint8_t *ports, int *n)
{
	*bus = libusb_get_bus_number(dev);
	*n = libusb_get_port_numbers(dev, ports, 8);
	return *n < 0 ? -1 : 0;
}

/* Looks for a device in accessory mode (optionally on the given bus path). */
static libusb_device *find_accessory(libusb_device **list, ssize_t n, int have_path, uint8_t bus, const uint8_t *ports, int np)
{
	for (ssize_t i = 0; i < n; i++) {
		struct libusb_device_descriptor d;
		if (libusb_get_device_descriptor(list[i], &d) || !is_accessory(&d)) continue;
		if (have_path) {
			uint8_t b, p[8];
			int k;
			if (path_of(list[i], &b, p, &k) || b != bus || k != np || memcmp(p, ports, (size_t)np)) continue;
		}
		return list[i];
	}
	return NULL;
}

static int claim(struct usblink *l, libusb_device *dev)
{
	struct libusb_config_descriptor *cfg;
	int r = libusb_get_active_config_descriptor(dev, &cfg);
	if (r) return -EIO;
	int found = 0;
	for (int i = 0; i < cfg->bNumInterfaces && !found; i++) {
		const struct libusb_interface_descriptor *d = &cfg->interface[i].altsetting[0];
		if (d->bInterfaceClass != 0xff || d->bInterfaceSubClass == 0x42 /* adb */ || d->bNumEndpoints < 2) continue;
		unsigned char in = 0, out = 0;
		int mps = 512;
		for (int e = 0; e < d->bNumEndpoints; e++) {
			const struct libusb_endpoint_descriptor *ep = &d->endpoint[e];
			if ((ep->bmAttributes & 3) != LIBUSB_TRANSFER_TYPE_BULK) continue;
			if (ep->bEndpointAddress & 0x80) in = ep->bEndpointAddress;
			else { out = ep->bEndpointAddress; mps = ep->wMaxPacketSize; }
		}
		if (in && out) { l->iface = d->bInterfaceNumber; l->ep_in = in; l->ep_out = out; l->max_out = mps; found = 1; }
	}
	libusb_free_config_descriptor(cfg);
	if (!found) return -ENODEV;
	libusb_set_auto_detach_kernel_driver(l->h, 1);
	return libusb_claim_interface(l->h, l->iface) ? -EBUSY : 0;
}

static void *in_thread(void *arg)
{
	struct usblink *l = arg;
	unsigned char buf[CHUNK];
	while (!atomic_load(&l->stop)) {
		int n = 0;
		int r = libusb_bulk_transfer(l->h, l->ep_in, buf, sizeof buf, &n, 200);
		if (n > 0) { /* a timeout can still carry data (a transfer that ended on an exact packet multiple) */
			unsigned char *p = buf;
			while (n > 0) {
				ssize_t w = write(l->in_pipe[1], p, (size_t)n);
				if (w < 0) { if (errno == EINTR) continue; goto done; }
				p += w;
				n -= (int)w;
			}
		}
		if (r == LIBUSB_ERROR_TIMEOUT) continue;
		if (r == LIBUSB_ERROR_OVERFLOW) continue;
		if (r != 0) break; /* NO_DEVICE, IO, PIPE: the link is over */
	}
done:
	atomic_store(&l->gone, 1);
	close(l->in_pipe[1]); /* EOF for the reader */
	l->in_pipe[1] = -1;
	return NULL;
}

static void *out_thread(void *arg)
{
	struct usblink *l = arg;
	unsigned char buf[CHUNK];
	for (;;) {
		struct pollfd pf = { .fd = l->out_pipe[0], .events = POLLIN };
		if (poll(&pf, 1, 100) < 0 && errno != EINTR) break;
		if (atomic_load(&l->stop)) break;
		if (!(pf.revents & (POLLIN | POLLHUP))) continue;
		ssize_t n = read(l->out_pipe[0], buf, sizeof buf);
		if (n <= 0) break;
		int off = 0;
		while (off < n && !atomic_load(&l->stop)) {
			int sent = 0;
			int r = libusb_bulk_transfer(l->h, l->ep_out, buf + off, (int)n - off, &sent, 200);
			off += sent;
			if (r == LIBUSB_ERROR_TIMEOUT) continue; /* the device is not reading yet (its app has not opened the accessory) */
			if (r != 0) goto done;
		}
		if (n % l->max_out == 0) { /* an exact packet multiple needs a zero-length packet to end the transfer */
			int sent;
			libusb_bulk_transfer(l->h, l->ep_out, buf, 0, &sent, 200);
		}
	}
done:
	atomic_store(&l->gone, 1);
	return NULL;
}

int usblink_open(int timeout_ms, unsigned vid_filter, const volatile int *stop, struct usblink **out, int *rd_fd, int *wr_fd)
{
	struct usblink *l = calloc(1, sizeof *l);
	if (!l) return -ENOMEM;
	l->in_pipe[0] = l->in_pipe[1] = l->out_pipe[0] = l->out_pipe[1] = -1;
	if (libusb_init(&l->ctx)) { free(l); return -EIO; }

	struct timespec t0;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	int have_path = 0, switched = 0;
	uint8_t bus = 0, ports[8];
	int np = 0;
	libusb_device *target = NULL;
	libusb_device **list = NULL;
	int err = -ETIMEDOUT;
	for (;;) {
		if (stop && *stop) { err = -ECANCELED; break; }
		ssize_t n = libusb_get_device_list(l->ctx, &list);
		if (n < 0) { err = -EIO; break; }
		target = find_accessory(list, n, have_path, bus, ports, np);
		if (!target && !switched && last_switch_us && mono_us() - last_switch_us < 5000000) switched = 1; /* asked a moment ago: wait for it */
		if (!target && !switched) { /* ask any Android device to switch */
			for (ssize_t i = 0; i < n && !switched; i++) {
				struct libusb_device_descriptor d;
				if (libusb_get_device_descriptor(list[i], &d) || d.bDeviceClass == LIBUSB_CLASS_HUB) continue;
				if (vid_filter && d.idVendor != vid_filter) continue;
				libusb_device_handle *h;
				int oe = libusb_open(list[i], &h);
				if (oe) { DBG("%04x:%04x: cannot open: %s", d.idVendor, d.idProduct, libusb_error_name(oe)); continue; } /* no permission (see contrib/udev) or not ours */
				int se = switch_to_accessory(h);
				DBG("%04x:%04x: switch to accessory mode: %s", d.idVendor, d.idProduct, se ? strerror(-se) : "requested");
				if (se == 0) {
					switched = 1;
					if (++ignored_switches >= 3 && !warned_switch) { /* three requests in a row went unanswered (a phone usually needs a few seconds) */
						fprintf(stderr, "usblink: the phone %04x:%04x did not switch to accessory mode: on it, choose File transfer in the USB notification\n", d.idVendor, d.idProduct);
						warned_switch = 1;
					}
					last_switch_us = mono_us();
					have_path = path_of(list[i], &bus, ports, &np) == 0;
					snprintf(l->desc, sizeof l->desc, "%04x:%04x", d.idVendor, d.idProduct);
				}
				libusb_close(h);
			}
		}
		if (target) { warned_switch = 0; ignored_switches = 0; break; }
		libusb_free_device_list(list, 1);
		list = NULL;
		struct timespec t1;
		clock_gettime(CLOCK_MONOTONIC, &t1);
		long waited = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
		if (timeout_ms >= 0 && waited >= timeout_ms) { err = -ETIMEDOUT; break; }
		msleep(switched ? 100 : 500);
	}
	if (!target) { if (list) libusb_free_device_list(list, 1); libusb_exit(l->ctx); free(l); return err; }

	struct libusb_device_descriptor d;
	libusb_get_device_descriptor(target, &d);
	uint8_t b, p[8];
	int k = 0;
	path_of(target, &b, p, &k);
	snprintf(l->desc, sizeof l->desc, "%04x:%04x on bus %u port %u", d.idVendor, d.idProduct, b, k ? p[k - 1] : 0);
	if (libusb_open(target, &l->h)) { err = -EACCES; goto fail; }
	libusb_free_device_list(list, 1);
	list = NULL;
	if ((err = claim(l, libusb_get_device(l->h))) != 0) goto fail;
	if (pipe(l->in_pipe) || pipe(l->out_pipe)) { err = -errno; goto fail; }
	if (pthread_create(&l->t_in, NULL, in_thread, l) || pthread_create(&l->t_out, NULL, out_thread, l)) { err = -EAGAIN; goto fail; }
	l->started = 1;
	*out = l;
	*rd_fd = l->in_pipe[0];
	*wr_fd = l->out_pipe[1];
	return 0;
fail:
	if (list) libusb_free_device_list(list, 1);
	usblink_close(l);
	return err;
}

int usblink_nudge(unsigned vid_filter)
{
	uint64_t now = mono_us();
	if (last_switch_us && now - last_switch_us < 10000000) return 0;
	libusb_context *ctx;
	if (libusb_init(&ctx)) return -EIO;
	libusb_device **list;
	ssize_t n = libusb_get_device_list(ctx, &list);
	int result = 0;
	if (n < 0) { libusb_exit(ctx); return -EIO; }
	if (find_accessory(list, n, 0, 0, NULL, 0)) { libusb_free_device_list(list, 1); libusb_exit(ctx); return 0; } /* already in accessory mode: the app has it or will */
	for (ssize_t i = 0; i < n && !result; i++) {
		struct libusb_device_descriptor d;
		if (libusb_get_device_descriptor(list[i], &d) || d.bDeviceClass == LIBUSB_CLASS_HUB) continue;
		if (vid_filter && d.idVendor != vid_filter) continue;
		libusb_device_handle *h;
		if (libusb_open(list[i], &h)) continue; /* no permission (see contrib/udev) or not ours */
		if (switch_to_accessory(h) == 0) { result = 1; last_switch_us = now; }
		libusb_close(h);
	}
	libusb_free_device_list(list, 1);
	libusb_exit(ctx);
	return result;
}

int usblink_gone(const struct usblink *l)
{
	return atomic_load(&l->gone);
}

const char *usblink_description(const struct usblink *l)
{
	return l->desc;
}

void usblink_close(struct usblink *l)
{
	if (!l) return;
	atomic_store(&l->stop, 1);
	if (l->started) {
		pthread_join(l->t_in, NULL);
		pthread_join(l->t_out, NULL);
	}
	if (l->h) {
		libusb_release_interface(l->h, l->iface);
		libusb_close(l->h);
	}
	for (int i = 0; i < 2; i++) {
		if (l->in_pipe[i] >= 0) close(l->in_pipe[i]);
		if (l->out_pipe[i] >= 0) close(l->out_pipe[i]);
	}
	libusb_exit(l->ctx);
	free(l);
}
