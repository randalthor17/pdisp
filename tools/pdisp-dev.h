/* The subcommands of `pdisp-dev` (tools/pdisp-dev.c): the development and diagnostic tools. */
#ifndef PDISP_DEV_H
#define PDISP_DEV_H

int edid_main(int argc, char **argv);        /* edid: generate and print an EDID */
int vdisplay_main(int argc, char **argv);    /* vdisplay: create a virtual output and hold it */
int capture_main(int argc, char **argv);     /* capture: grab frames of the virtual output (PNG) */
int encode_main(int argc, char **argv);      /* encode: the capture and encode pipeline on its own */
int evdump_main(int argc, char **argv);      /* evdump: print evdev events of a device (root) */
int touch_demo_main(int argc, char **argv);  /* touch-demo: create the virtual touchscreen and tap (root) */

#endif
