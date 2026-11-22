# Security design (M6)

pdisp streams a computer's screen to a phone and, later, sends touch back. Anyone who can connect and be accepted can see the
screen and inject input, so a device is only served after a **cryptographic handshake** and an **authorisation decision**, and
everything after that is **encrypted and authenticated**. This is modelled on adb: the phone has a key pair, the PC keeps a list of
keys it trusts, the first connection needs an approval on the PC, and later ones do not.

## Threat model
In scope: other devices on the same Wi-Fi (they can connect to the port, sniff, replay, inject, or pose as either side), a hostile
hotspot or router, someone holding a copy of captured traffic. Out of scope: an attacker who controls the PC or the phone (they
have the keys), and traffic analysis (packet sizes and timing reveal that a stream exists and roughly how much changes on screen).

## Identities and files
- Each side has a long-term **X25519** key pair, created on first use: the PC's in `~/.config/pdisp/identity`, the phone's in the app's
  private storage. The file is 64 hex digits, mode 0600 in a 0700 directory; a key file other users can read is refused, not fixed.
- The PC keeps `~/.config/pdisp/authorized_keys`: one line per device, the public key in hex, then its name. Hand-editable;
  comments and malformed lines are ignored. The phone keeps `known_pcs` in the same format. `pdisp keys list | remove | identity`
  manages the PC's side. The daemon reads the file for each new connection, so a revoked device is refused immediately.
- A **fingerprint** (80 bits of the key's BLAKE2b hash, `xxxx-xxxx-xxxx-xxxx-xxxx`) is how keys are shown to people and logs.

## The handshake
`Noise_XX_25519_ChaChaPoly_BLAKE2b` (https://noiseprotocol.org), implemented in `proto/noise.c` over Monocypher's primitives
(`third_party/monocypher`, public domain / BSD). The phone is the initiator:

    -> e
    <- e, ee, s, es          (payload: the PC's name)
    -> s, se                 (payload: the phone's name)

Both sides end up with the other's **authenticated static key**, forward-secret keys, and the same **handshake hash**. The messages
travel as ordinary control frames of type HANDSHAKE, the only ones that are not encrypted. The prologue is `pdisp/1`.
`noise_finish` derives, from the final chaining key, four keys: the control channel in each direction (the standard Noise split) and the
datagram channel in each direction (a separate derivation, so no key is ever used for both).

Verified by `tests/test_noise.c`: byte-exact against an independent Python implementation of the same Noise spec (messages, hash,
all keys), plus every failure: a wrong prologue, any single flipped byte of messages 2 and 3, truncation, a low-order ephemeral
key, wrong order, replayed message. The primitives are checked against RFC 8439, RFC 7748 and BLAKE2b/HMAC vectors from independent
implementations (`tests/test_crypto.c`).

## Authorisation and pairing
After the handshake the phone sends HELLO (now encrypted) and the PC decides from the phone's key:
- **In `authorized_keys`**: continue, no prompt.
- **Unknown**: depends on `pdispd -P`:
  - `ask` (default): the PC sends PAIRING_REQUIRED and prints the device's name, key fingerprint and a **six-digit code**, then waits
    (90 s) for `y` (allow, remember), `o` (this time only) or anything else (refuse). The phone shows its own copy of the code.
    The code is a hash of the handshake transcript: **if someone sits in the middle, the two sides computed different transcripts and
    the codes differ**, which is the person's cue to answer no. The person is the second factor.
  - `deny`: reply REJECT (not authorised). `accept`: add the key without asking (development, logged as a warning).
- The phone remembers the PC's key in `known_pcs` once the PC lets it in. (The phone trusts a PC on first use; the pairing code is the
  protection against a middle-man at that moment. A phone that talks to a PC whose key it has not seen shows the code again.)
- A stranger that fails or abandons the handshake is dropped after at most 10 s and does not stop the daemon. Rejection leaves no
  monitor behind: the virtual output is created only after authorisation.

## Encrypted transport
- **Control channel**: every control frame is sealed as `u32 length | ChaCha20-Poly1305(frame) | tag` with the length as associated
  data and a per-direction counter as the nonce. A dropped, repeated, reordered, truncated or modified frame fails authentication and the
  connection ends; frames that authenticated before the bad one are still delivered. (`proto/secure.c`, tested with a byte-level
  man in the middle in `tests/test_secure.c`.)
- **Datagrams**: the 16-byte header stays readable and is the associated data, the payload is encrypted, a 16-byte tag follows. The
  nonce is a direction byte and the header's 64-bit sequence number. The receiver keeps a 1024-packet replay window (reordering and loss
  are normal for UDP, replays are refused before any cryptography is spent on them) and only authenticated packets move it.
- **Nonce discipline**: all of one side's datagrams (video, feedback, time sync, keepalive, the phone's HELLO_UDP) draw from one
  sequence space. A **new epoch** (RECONFIGURE) restarts the sequence at 0, so each epoch has its own keys derived from the handshake keys
  (`sec_udp_set_epoch`); without this the same (key, nonce) pair would be used twice, which would leak plaintext.
- Over USB the same handshake and the same sealing run inside the byte pipe, so a cable is not trusted any more than Wi-Fi.
- Unauthenticated datagrams never reach the protocol code; the daemon counts them (`auth_drops`).

## Development switches
`pdispd -I` and `pdisp-view -I` turn security off (the M4 behaviour) for experiments; the daemon logs a warning, and a secure client refuses
to talk to it (the handshake fails). Tests use `-P accept` with throw-away identity directories.

## Known limits
- The phone's key is a file in app-private storage, not hardware-backed (Android Keystore cannot hold an X25519 key before API 33 and
  we need one on API 29). Wrapping it with a Keystore key is a later hardening step.
- The prompt is a line on the daemon's stdin. A desktop notification with buttons belongs with the user-service packaging (M9, M11).
- The pairing code protects only if the person compares it. Device names are shown to the person but are not authenticated beyond
  the key.
- One handshake at a time: a peer that connects and says nothing occupies the daemon for up to 10 s.
- No rekeying inside a session (a 64-bit sequence space does not run out) and no revocation of a running session (the next
  connection is refused).
