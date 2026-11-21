// THROWAWAY spike (M0): does the phone/tablet support Android Open Accessory (AOA)?
// Build: gcc -O2 aoa_spike.c $(pkg-config --cflags --libs libusb-1.0) -o aoa_spike
// Run:   ./aoa_spike [vid_hex]   (default: first device that answers AOA GET_PROTOCOL)
#include <libusb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define AOA_GET_PROTOCOL 51
#define AOA_SEND_STRING 52
#define AOA_START 53

static int send_str(libusb_device_handle *h, int idx, const char *s)
{
	return libusb_control_transfer(h, LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_ENDPOINT_OUT, AOA_SEND_STRING, 0, idx,
				       (unsigned char *)s, strlen(s) + 1, 1000);
}

static void dump_endpoints(libusb_device *dev)
{
	struct libusb_config_descriptor *cfg;
	if (libusb_get_active_config_descriptor(dev, &cfg)) return;
	for (int i = 0; i < cfg->bNumInterfaces; i++)
		for (int a = 0; a < cfg->interface[i].num_altsetting; a++) {
			const struct libusb_interface_descriptor *d = &cfg->interface[i].altsetting[a];
			printf("  interface %d class=%02x sub=%02x proto=%02x eps=%d\n", d->bInterfaceNumber,
			       d->bInterfaceClass, d->bInterfaceSubClass, d->bInterfaceProtocol, d->bNumEndpoints);
			for (int e = 0; e < d->bNumEndpoints; e++)
				printf("    ep 0x%02x %s maxpacket=%d\n", d->endpoint[e].bEndpointAddress,
				       (d->endpoint[e].bmAttributes & 3) == 2 ? "bulk" : "other", d->endpoint[e].wMaxPacketSize);
		}
	libusb_free_config_descriptor(cfg);
}

int main(int argc, char **argv)
{
	int want_vid = argc > 1 ? (int)strtol(argv[1], NULL, 16) : 0;
	libusb_context *ctx;
	libusb_init(&ctx);
	libusb_device **list;
	ssize_t n = libusb_get_device_list(ctx, &list);
	libusb_device_handle *h = NULL;
	for (ssize_t i = 0; i < n; i++) {
		struct libusb_device_descriptor dd;
		libusb_get_device_descriptor(list[i], &dd);
		if (want_vid && dd.idVendor != want_vid) continue;
		if (!want_vid && (dd.bDeviceClass == LIBUSB_CLASS_HUB)) continue;
		libusb_device_handle *t;
		if (libusb_open(list[i], &t)) continue;
		unsigned char ver[2] = { 0, 0 };
		int r = libusb_control_transfer(t, LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_ENDPOINT_IN, AOA_GET_PROTOCOL, 0, 0, ver, 2, 1000);
		printf("%04x:%04x GET_PROTOCOL -> %d, version %d\n", dd.idVendor, dd.idProduct, r, r == 2 ? ver[0] | ver[1] << 8 : -1);
		if (r == 2 && (ver[0] | ver[1] << 8) >= 1) { h = t; break; }
		libusb_close(t);
	}
	if (!h) { printf("no AOA-capable device found\n"); return 1; }

	send_str(h, 0, "pdisp");
	send_str(h, 1, "pdisp-spike");
	send_str(h, 2, "pdisp M0 spike");
	send_str(h, 3, "0.0");
	send_str(h, 4, "https://example.invalid/pdisp");
	send_str(h, 5, "0");
	int r = libusb_control_transfer(h, LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_ENDPOINT_OUT, AOA_START, 0, 0, NULL, 0, 1000);
	printf("START -> %d (%s)\n", r, r < 0 ? libusb_error_name(r) : "ok");
	libusb_close(h);

	// the device drops off the bus and comes back as Google 18d1:2d00/2d01 (accessory [+adb])
	for (int t = 0; t < 50; t++) {
		usleep(100000);
		libusb_device **l2;
		ssize_t m = libusb_get_device_list(ctx, &l2);
		for (ssize_t i = 0; i < m; i++) {
			struct libusb_device_descriptor dd;
			libusb_get_device_descriptor(l2[i], &dd);
			if (dd.idVendor == 0x18d1 && (dd.idProduct & 0xFFF0) == 0x2D00) {
				printf("re-enumerated as %04x:%04x after %d ms, speed=%d\n", dd.idVendor, dd.idProduct, (t + 1) * 100,
				       libusb_get_device_speed(l2[i]));
				dump_endpoints(l2[i]);
				libusb_free_device_list(l2, 1);
				libusb_free_device_list(list, 1);
				libusb_exit(ctx);
				return 0;
			}
		}
		libusb_free_device_list(l2, 1);
	}
	printf("device did not re-enumerate in accessory mode within 5 s\n");
	libusb_free_device_list(list, 1);
	libusb_exit(ctx);
	return 2;
}
