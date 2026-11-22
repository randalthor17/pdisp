# Finding the PC on the network (mDNS)

`pdispd` announces itself with mDNS / DNS-SD (RFC 6762, 6763) so the app lists the PC without typing its address. No avahi is needed: the daemon has
a small responder of its own (`daemon/mdns.c`); avahi or systemd-resolved can run beside it (they share UDP 5353).

| | |
|---|---|
| Service type | `_pdisp._tcp` |
| Instance | `pdisp on <hostname>` |
| Host | `pdisp-<hostname>.local` (an A record for the address on the network the query came from) |
| Port | the daemon's `-p` port (47110) |
| TXT | `v=1`, `fp=<the PC's key fingerprint>` (not secret: the pairing dialog shows the same) |

- It announces when the daemon starts listening on the network, every 60 s, and says goodbye (TTL 0) when it stops. It answers queries on every IPv4 network with multicast
  (re-scanned every 5 s, so Wi-Fi coming up later is picked up). `-M` turns it off.
- It never queries or probes, so a clash of host names is not detected. The host label starts with `pdisp-` to avoid the machine's own name.
- **Firewall:** queries from the phone come in on UDP 5353. The NixOS module opens it with `openFirewall`; elsewhere open UDP 5353 as well as the port.
- **App:** while the PC list is showing, the app browses `_pdisp._tcp` (Android's NsdManager). A PC not in the list yet appears under "Found on your network";
  one tap adds it and connects (the PC asks to approve the device the first time). Discovery stops when streaming starts. A PC on another port is stored as `host:port`.
- Not covered: IPv6, several PCs with the same host name, discovery over USB (not needed), and Android 17's local-network permission (to be added when it becomes mandatory).

Tested: `tests/test_mdns.c` (the packets decode as DNS, only matching queries are answered).
