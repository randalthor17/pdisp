/* Carries the control stream and the datagram channel over one reliable byte pipe (USB accessory bulk endpoints), so the code on
 * either side keeps using ordinary sockets (docs/protocol.md section 1).
 *
 * usbmux_start() takes the two ends of the pipe (link_rd, link_wr; they may be the same descriptor) and returns two local
 * descriptors for the application:
 *   ctl:   a SOCK_STREAM socket, a drop-in for the TCP control connection
 *   dgram: a SOCK_DGRAM socket, a drop-in for the connected UDP socket (one send() or recv() is one datagram)
 *
 * Link framing: u32 length (bytes after this field, little-endian), u8 kind (0 = control stream bytes, 1 = datagram), payload.
 * Control bytes are chunked as read; datagrams keep their boundaries. Nothing is parsed, dropped or retransmitted: the pipe is reliable.
 */
#ifndef PDISP_USBMUX_H
#define PDISP_USBMUX_H

#include <stddef.h>
#include <stdint.h>

#define USBMUX_KIND_CTL 0
#define USBMUX_KIND_DGRAM 1
#define USBMUX_MAX_FRAME (1u << 20)

struct usbmux;

/* Returns 0 with the mux and the application's descriptors, or a negative errno. */
int usbmux_start(int link_rd, int link_wr, struct usbmux **out, int *ctl, int *dgram);

/* Stops the threads and closes the local descriptors the mux kept (not link_rd, link_wr and not the application's ends). */
void usbmux_stop(struct usbmux *m);

/* True once the link has failed or been closed. */
int usbmux_link_down(const struct usbmux *m);

#endif
