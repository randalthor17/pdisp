/* USB link to an Android device in accessory mode (AOA, https://source.android.com/docs/core/interaction/accessories/aoa).
 *
 * usblink_open() waits for a device, switches it to accessory mode if it is not already, claims its bulk endpoints and returns
 * two descriptors carrying the bytes of the link (feed them to usbmux_start()). When the device is unplugged both directions
 * end: reads return EOF.
 */
#ifndef PDISP_USBLINK_H
#define PDISP_USBLINK_H

struct usblink;

/* Waits up to `timeout_ms` (-1 = forever, 0 = look once) for an Android device. `vid_filter` limits the search to one vendor
 * (0 = any). Returns 0 with the link and descriptors, -ETIMEDOUT if no device appeared, or another negative errno.
 * `*stop` (if not NULL) is polled while waiting; a non-zero value makes the call return -ECANCELED. */
int usblink_open(int timeout_ms, unsigned vid_filter, const volatile int *stop, struct usblink **out, int *rd_fd, int *wr_fd);

/* Stops the transfer threads and releases the device. The descriptors handed out are closed. */
void usblink_close(struct usblink *l);

/* True after the device went away or a transfer failed for good. */
int usblink_gone(const struct usblink *l);

/* "vid:pid on bus-port" of the open device, for logs. */
const char *usblink_description(const struct usblink *l);

#endif
