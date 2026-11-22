/* The subcommands of the `pdisp` command (tools/pdisp.c). Each takes its own argv, where argv[0] is the name to show in messages. */
#ifndef PDISP_CLI_H
#define PDISP_CLI_H

int ctl_main(int argc, char **argv);        /* status, watch, devices, remove, set, disconnect, pairing, answer: the daemon's control socket */
int keys_main(int argc, char **argv);       /* keys list | remove | identity: the authorised devices file */
int brightness_main(int argc, char **argv); /* brightness: the phone's brightness over DDC/CI */

#endif
