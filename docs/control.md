# The control socket

`pdispd` listens on a Unix socket so a GUI, a tray applet or a script can see what it is doing and ask it to do things. The pdisp GUI uses it, and
`pdisp-ctl` is a small command-line client for it.

- **Where**: `$XDG_RUNTIME_DIR/pdisp/control.sock` (mode 0600 in a 0700 directory). `-C PATH` chooses another path, `-C none` turns it off. Only the
  same user may connect (`SO_PEERCRED`). If another daemon of that user is already listening on the path, a second daemon leaves it alone and runs without
  a control socket (so a test daemon cannot take over the real one).
- **Threading**: the server has its own thread. The streaming loop copies a snapshot into it about once a second and never waits for a client.
- **Format**: one JSON object per line, both ways. The commands are flat objects of strings, numbers and booleans.

## Events (daemon to client)
| event | when | fields |
|---|---|---|
| `hello` | on connect | `version` ("1") |
| `state` | on connect, on a change, otherwise at most 4 times a second while numbers move | `usb`, `net`, `port`, `usb_state` (`off`, `waiting`, `accessory`), `session` (null or `device`, `transport` (`usb`, `network`), `width`, `height`, `refresh_hz`, `seconds`, `fps` (pictures sent per second, refinement included), `bitrate_mbps`, `queue_ms`, `phone_queue_ms`, `phone_decode_ms`, `frames_lost`, `idr_frames`, `repair_packets`), `monitor` (null or `connector`, `grace`: kept for a lost device), `config` (`refresh_cap_hz`, `bitrate_kbps`, `max_bitrate_kbps`, `grace_secs`, `refine_frames`) |
| `pairing` | an unknown device asks to be trusted, to clients that sent `subscribe` with `"pairing":true` | `id`, `name`, `fingerprint`, `code` (six digits: the device shows the same) |
| `pairing_done` | the request was decided or timed out | `id`, `result` (`always`, `once`, `deny`, `timeout`) |

## Commands (client to daemon)
Every command may carry a `"req"` string, which comes back in the reply. A reply is `{"reply":"<cmd>","ok":true|false,"req":...,"error":"..."}` plus fields.

| command | effect |
|---|---|
| `{"cmd":"get_state"}` | reply, then a `state` event |
| `{"cmd":"subscribe","pairing":true}` | this client receives pairing requests; with a client subscribed the daemon asks the GUI and not the prompt command (`-A`) or the terminal. Without one it falls back to those. |
| `{"cmd":"pair_answer","id":7,"answer":"always"}` | `always`, `once` or `deny`; refused if the request is gone or already answered |
| `{"cmd":"disconnect"}` | end the session and remove the virtual monitor now (also while it waits for a lost device) |
| `{"cmd":"devices"}` | reply with `devices`: `[{"fingerprint":"xxxx-xxxx-xxxx-xxxx-xxxx","name":"..."}]` |
| `{"cmd":"remove_device","fingerprint":"..."}` | revoke by prefix (at least 4 characters); reply `removed` |
| `{"cmd":"set_config","key":"refresh_cap_hz","value":"90"}` | validated, applied from the next session, and saved in `pdispd.conf` next to the keys; keys as in `config` above |

The settings file `pdispd.conf` (`key=value` lines in the config directory) is read at start; an option given on the command line (`-R`, `-b`, `-B`, `-g`, `-F`) wins.

## pdisp-ctl
`pdisp-ctl status | watch [N] | devices | remove FINGERPRINT | set KEY VALUE | disconnect | pairing [SECS] | answer ID always|once|deny` (`-s SOCKET` to choose the
socket). Exit status 0 if the daemon said ok. `tests/m11_control_check.sh` drives all of it against a real daemon with a viewer as the device.

## Not in it yet
Streaming the daemon's log, changing options that need a restart (USB on or off, the port, the pairing mode), and several devices at once (there is one at a time).
