#!/usr/bin/env bash
# M6 check: pairing, authorisation and encryption through the real daemon and the desktop test client (the phone's stand-in).
#   - an unknown device is rejected when the daemon denies, and nothing is stored
#   - the first connection can be approved (accept, or the ask prompt), and the key is remembered
#   - an authorised key reconnects with no prompt (the daemon is set to deny everything unknown)
#   - a different key is rejected; the pairing code both sides show is the same
#   - plain clients and a secure client against an insecure daemon fail; the daemon survives strangers
#   - a world-readable identity file is refused
# Needs niri, a free evdi device and a VAAPI HEVC encoder (the streaming cases). Exits 77 (meson: skipped) otherwise.
# usage: m6_pairing_check.sh <pdispd> <pdisp-view> [artifact-dir] [pdisp]
set -u
daemon=$1; viewer=$2
art=${3:-$(mktemp -d)}; keys=${4:-}; mkdir -p "$art"; rm -rf "$art"/*
skip() { echo "SKIP: $*"; exit 77; }
command -v niri >/dev/null 2>&1 || skip "niri not installed"
niri msg --json outputs >/dev/null 2>&1 || skip "no running niri session"

PORT=${PDISP_TEST_PORT:-24116}
dpid=
cleanup() { [ -n "$dpid" ] && kill "$dpid" 2>/dev/null; wait 2>/dev/null; }
trap cleanup EXIT
fail() { echo "FAIL: $*"; echo "--- daemon:"; grep -v '^\[libevdi\|^libva' "$art/daemon.log" 2>/dev/null | tail -12; echo "--- viewer:"; tail -8 "$art/viewer.log" 2>/dev/null; exit 1; }
rm -rf "$art/pc" "$art/phoneA" "$art/phoneB"; mkdir -p "$art/pc" "$art/phoneA" "$art/phoneB"; chmod 700 "$art/pc"

start_daemon() { # start_daemon <mode> [extra args...]; the daemon serves one session and exits (-o) unless asked otherwise
	local mode=$1; shift
	"$daemon" -p "$PORT" -K "$art/pc" -P "$mode" "$@" >"$art/daemon.log" 2>&1 &
	dpid=$!
	local i=0; while [ $i -lt 40 ] && ! grep -q "listening on port" "$art/daemon.log"; do
		kill -0 "$dpid" 2>/dev/null || { wait "$dpid"; [ $? = 3 ] && skip "no free evdi device"; fail "daemon exited early"; }
		sleep 0.25; i=$((i + 1)); done
}
run_viewer() { # run_viewer <identity dir> [extra args...]; prints the exit status in $vrc
	local id=$1; shift
	timeout 40 "$viewer" 127.0.0.1 "$PORT" -i "$id" -S 3 "$@" >"$art/viewer.log" 2>&1
	vrc=$?
}
stop_daemon() { [ -n "$dpid" ] && { kill "$dpid" 2>/dev/null; wait "$dpid" 2>/dev/null; }; dpid=; }
streams() { grep -q '^VIEW' "$art/viewer.log" && [ "$(sed -n 's/^VIEW .* decoded=\([0-9]*\) .*/\1/p' "$art/viewer.log")" -ge 1 ]; }
nkeys() { [ -f "$art/pc/authorized_keys" ] && grep -c '^[0-9a-f]\{64\}' "$art/pc/authorized_keys" || echo 0; }
viewer_code() { sed -n 's/^PAIRING code=\([0-9]*\) .*/\1/p' "$art/viewer.log" | head -1; }
daemon_code() { sed -n 's/.*pairing code \([0-9]*\).*/\1/p' "$art/daemon.log" | head -1; }

# 1. unknown device, daemon denies: rejected, nothing stored, and the daemon did not start a stream
start_daemon deny -o
run_viewer "$art/phoneA"
[ "$vrc" = 5 ] || fail "an unknown device was not rejected (viewer exit $vrc)"
grep -q "rejected (2)" "$art/viewer.log" || fail "the viewer did not learn why it was rejected"
[ "$(nkeys)" = 0 ] || fail "a rejected device was stored"
grep -q "virtual output" "$art/daemon.log" && fail "the daemon built a monitor for an unauthorised device"
stop_daemon
echo "ok 1: unknown device rejected, nothing stored"

# 2. the first connection is approved (accept mode): it streams, the key is stored, both sides showed the same code
start_daemon accept -o
run_viewer "$art/phoneA"
[ "$vrc" = 0 ] || fail "the approved device failed (exit $vrc)"
streams || fail "no video after pairing"
[ "$(nkeys)" = 1 ] || fail "the key was not stored ($(nkeys) keys)"
[ -n "$(viewer_code)" ] && [ "$(viewer_code)" = "$(daemon_code)" ] || fail "the pairing codes differ: phone '$(viewer_code)', PC '$(daemon_code)'"
grep -q "new_pc=1" "$art/viewer.log" || fail "the viewer should have reported a PC it had not paired before"
grep -q "auth_drops=0" "$art/viewer.log" || fail "datagrams were dropped as unauthentic in a normal run"
stop_daemon
echo "ok 2: approved, streamed, stored, codes match ($(viewer_code))"

# 3. the authorised key reconnects with no prompt, although the daemon now denies everything unknown
start_daemon deny -o
run_viewer "$art/phoneA"
[ "$vrc" = 0 ] || fail "the authorised device could not reconnect (exit $vrc)"
streams || fail "no video on reconnect"
grep -q "authorised device" "$art/daemon.log" || fail "the daemon did not recognise the device"
grep -q "new_pc=0" "$art/viewer.log" || fail "the viewer forgot the PC"
stop_daemon
echo "ok 3: authorised device reconnected without a prompt"

# 4. a different key is rejected
start_daemon deny -o
run_viewer "$art/phoneB"
[ "$vrc" = 5 ] || fail "an unauthorised second device was not rejected (exit $vrc)"
[ "$(nkeys)" = 1 ] || fail "key count changed"
stop_daemon
echo "ok 4: another key rejected"

# 5. the ask prompt: answering n, o and y
stop_daemon
ask() { # ask <answer> -> runs daemon with that answer on stdin and the viewer against it
	"$daemon" -p "$PORT" -K "$art/pc" -P ask -o < <(sleep 3; echo "$1") >"$art/daemon.log" 2>&1 &
	dpid=$!
	local i=0; while [ $i -lt 40 ] && ! grep -q "listening on port" "$art/daemon.log"; do sleep 0.25; i=$((i + 1)); done
	run_viewer "$art/phoneB"
	stop_daemon
}
ask n
[ "$vrc" = 5 ] && grep -q "rejected (5)" "$art/viewer.log" || fail "answering n did not deny (exit $vrc)"
grep -q "PAIRING_REQUIRED" "$art/viewer.log" || fail "the viewer was not told to wait for approval"
[ "$(viewer_code)" = "$(daemon_code)" ] || fail "ask: the codes differ"
[ "$(nkeys)" = 1 ] || fail "a denied device was stored"
ask o
[ "$vrc" = 0 ] && streams || fail "answering o (this time only) did not let it in (exit $vrc)"
[ "$(nkeys)" = 1 ] || fail "'this time only' stored the key"
ask y
[ "$vrc" = 0 ] && streams || fail "answering y did not let it in (exit $vrc)"
[ "$(nkeys)" = 2 ] || fail "'always' did not store the key"
start_daemon deny -o
run_viewer "$art/phoneB"
[ "$vrc" = 0 ] && streams || fail "the device approved with y could not reconnect"
stop_daemon
echo "ok 5: the prompt's n / o / y behave, and y is remembered"

# 5b. the prompt command (-A), as the systemd service uses it: exit 1 denies, 2 is this time only, 0 is for good; it sees the peer's name, key and code
mkdir -p "$art/phoneC"
ask_cmd() { # ask_cmd <shell command>
	"$daemon" -p "$PORT" -K "$art/pc" -P ask -A "$1" -o </dev/null >"$art/daemon.log" 2>&1 &
	dpid=$!
	local i=0; while [ $i -lt 40 ] && ! grep -q "listening on port" "$art/daemon.log"; do sleep 0.25; i=$((i + 1)); done
	run_viewer "$art/phoneC"
	stop_daemon
}
ask_cmd "echo \"\$PDISP_PEER_NAME \$PDISP_PAIR_CODE \$PDISP_PEER_KEY\" > \"$art/prompt.env\"; exit 1"
[ "$vrc" = 5 ] && grep -q "rejected (5)" "$art/viewer.log" || fail "-A: exit 1 did not deny (exit $vrc)"
[ "$(awk '{print $2}' "$art/prompt.env")" = "$(viewer_code)" ] || fail "-A: the command saw code '$(awk '{print $2}' "$art/prompt.env")', the device shows '$(viewer_code)'"
[ -n "$(awk '{print $1}' "$art/prompt.env")" ] && [ "$(awk '{print length($3)}' "$art/prompt.env")" -ge 16 ] || fail "-A: the command did not get the name and the key"
[ "$(nkeys)" = 2 ] || fail "-A: a denied device was stored"
ask_cmd "exit 2"
[ "$vrc" = 0 ] && streams || fail "-A: exit 2 did not let it in (exit $vrc)"
[ "$(nkeys)" = 2 ] || fail "-A: 'this time only' stored the key"
ask_cmd "exit 0"
[ "$vrc" = 0 ] && streams || fail "-A: exit 0 did not let it in (exit $vrc)"
[ "$(nkeys)" = 3 ] || fail "-A: 'always' did not store the key"
echo "ok 5b: the prompt command (-A): exit 1 denies, 2 is this time only, 0 is for good, and it sees name, key and code"

# 6. a stranger cannot take the daemon down: a plain client fails, a secure one still works afterwards
start_daemon deny
run_viewer "$art/phoneA" -I
[ "$vrc" != 0 ] || fail "a plain client got a session from a secure daemon"
kill -0 "$dpid" 2>/dev/null || fail "the daemon exited after a stranger"
timeout 3 bash -c "exec 3<>/dev/tcp/127.0.0.1/$PORT; sleep 1" 2>/dev/null # connects, says nothing, hangs up
kill -0 "$dpid" 2>/dev/null || fail "the daemon exited after a silent connection"
run_viewer "$art/phoneA"
[ "$vrc" = 0 ] && streams || fail "the daemon did not serve an authorised device after the strangers (exit $vrc)"
grep -q "failed the handshake" "$art/daemon.log" || fail "the daemon did not log the failed handshake"
stop_daemon
echo "ok 6: plain and silent strangers did not stop the daemon"

# 7. a secure client against a daemon running without security
start_daemon accept -o -I
run_viewer "$art/phoneA"
[ "$vrc" = 4 ] || fail "a secure client did not notice the insecure daemon (exit $vrc)"
stop_daemon
echo "ok 7: insecure daemon detected by a secure client"

# 9. revoking: list and remove with the tool; the removed device is refused at once, the other one still works
if [ -n "$keys" ]; then
	"$keys" keys -K "$art/pc" list >"$art/list.txt" 2>&1 || fail "pdisp keys list failed"
	[ "$(grep -c . "$art/list.txt")" = 3 ] || fail "the list should show three devices: $(cat "$art/list.txt")"
	"$keys" keys -K "$art/pc" identity | grep -q '^[0-9a-f]\{4\}-' || fail "pdisp keys identity printed no fingerprint"
	"$keys" keys -K "$art/pc" remove ab >/dev/null 2>&1 && fail "a 2-character prefix removed something"
	victim=$("$keys" keys -K "$art/phoneA" identity | cut -d' ' -f1)
	grep -q "$victim" "$art/list.txt" || fail "phone A is not in the list"
	"$keys" keys -K "$art/pc" remove "$victim" | grep -q "removed 1" || fail "pdisp keys remove did not remove the device"
	[ "$(nkeys)" = 2 ] || fail "after one removal $(nkeys) keys remain"
	start_daemon deny -o
	run_viewer "$art/phoneA"
	[ "$vrc" = 5 ] || fail "the revoked device was not refused (exit $vrc)"
	stop_daemon
	start_daemon deny -o
	run_viewer "$art/phoneB"
	[ "$vrc" = 0 ] && streams || fail "the device that was not revoked stopped working (exit $vrc)"
	stop_daemon
	echo "ok 9: devices listed and revoked with pdisp-keys"
fi

# 8. an identity file that other users can read is refused
chmod 644 "$art/pc/identity"
"$daemon" -p "$PORT" -K "$art/pc" -P deny -o >"$art/daemon.log" 2>&1; rc=$?
[ "$rc" = 1 ] && grep -q "cannot use the identity" "$art/daemon.log" || fail "a world-readable identity was accepted (exit $rc)"
chmod 600 "$art/pc/identity"
echo "ok 8: world-readable identity refused"
echo "PASS: unknown devices rejected, approval remembered, authorised devices reconnect without a prompt, strangers cannot disturb the daemon"
