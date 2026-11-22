#!/usr/bin/env bash
# M11 check: the control socket of pdispd (docs/control.md), through the real daemon, a viewer as the device and pdisp-ctl as the GUI.
#   state before and during a session; a pairing request answered through the socket (deny, then always); disconnect; the paired devices
#   list and revoking one; settings changed and saved; a bad value refused; a second daemon leaves the first one's socket alone.
# Needs niri, jq, a free evdi device and a VAAPI HEVC encoder. Exits 77 (meson: skipped) otherwise.
# usage: m11_control_check.sh <pdispd> <pdisp-view> <pdisp-ctl> [artifact-dir]
set -u
daemon=$1; viewer=$2; ctl=$3
art=${4:-$(mktemp -d)}; mkdir -p "$art"; rm -rf "$art"/*
skip() { echo "SKIP: $*"; exit 77; }
command -v niri >/dev/null 2>&1 && command -v jq >/dev/null 2>&1 || skip "niri or jq missing"
niri msg --json outputs >/dev/null 2>&1 || skip "no running niri session"
PORT=${PDISP_TEST_PORT:-47117}
sock=$(mktemp -u /tmp/pdisp-ctl-XXXXXX.sock) # a short path: Unix socket paths are limited to 107 characters
dpid=; vpid=; cpid=
cleanup() { for p in $cpid $vpid $dpid; do [ -n "$p" ] && kill "$p" 2>/dev/null; done; wait 2>/dev/null; rm -f "$sock"; }
trap cleanup EXIT
fail() { echo "FAIL: $*"; echo "--- daemon:"; grep -v '^\[libevdi\|^libva' "$art/daemon.log" 2>/dev/null | tail -12; exit 1; }
mkdir -p "$art/pc"; chmod 700 "$art/pc"
C() { "$ctl" -s "$sock" "$@"; }
state() { C status; }
wait_state() { # wait_state <jq expression that must be true> [seconds]
	local i=0 n=$(( ${2:-10} * 5 ))
	while [ $i -lt $n ]; do state 2>/dev/null | jq -e "$1" >/dev/null 2>&1 && return 0; sleep 0.2; i=$((i + 1)); done; return 1
}

"$daemon" -C "$sock" -K "$art/pc" -P ask -p "$PORT" -t PDISPCTL -n pdisp-test -g 5 >"$art/daemon.log" 2>&1 &
dpid=$!
i=0; while [ $i -lt 50 ] && ! [ -S "$sock" ]; do
	kill -0 "$dpid" 2>/dev/null || { wait "$dpid"; [ $? = 3 ] && skip "no free evdi device"; fail "daemon exited early"; }
	sleep 0.2; i=$((i + 1)); done
[ -S "$sock" ] || fail "no control socket"
[ "$(stat -c %a "$sock")" = 600 ] || fail "the socket is not private (mode $(stat -c %a "$sock"))"

# 1. the state before any device
st=$(state) || fail "status failed"
echo "$st" | jq -e '.event == "state" and .session == null and .monitor == null and .net == true and .usb == false and .port == '"$PORT"' and .config.refresh_cap_hz == 60' >/dev/null || fail "unexpected idle state: $st"
echo "ok 1: idle state: $st" | cut -c1-150

# 2. a second daemon must not take the socket over
"$daemon" -C "$sock" -K "$art/pc" -P deny -p $((PORT + 1)) -t PDISPCTL2 -o >"$art/daemon2.log" 2>&1 &
d2=$!
sleep 1.5
kill "$d2" 2>/dev/null; wait "$d2" 2>/dev/null
grep -q "no control socket" "$art/daemon2.log" || fail "a second daemon did not leave the first one's socket alone"
state | jq -e '.event == "state"' >/dev/null || fail "the first daemon's socket stopped working"
echo "ok 2: a second daemon leaves the socket alone"

# 3. pairing through the socket: deny first
mkdir -p "$art/phoneA" "$art/phoneB"
C pairing 25 >"$art/pair1.out" 2>&1 &
cpid=$!
sleep 0.5
timeout 40 "$viewer" 127.0.0.1 "$PORT" -i "$art/phoneA" -S 6 >"$art/viewerA.log" 2>&1 &
vpid=$!
i=0; while [ $i -lt 50 ] && ! grep -q '"event":"pairing"' "$art/pair1.out" 2>/dev/null; do sleep 0.2; i=$((i + 1)); done
grep -q '"event":"pairing"' "$art/pair1.out" || fail "no pairing request reached the GUI"
req=$(grep '"event":"pairing"' "$art/pair1.out" | head -1)
id=$(echo "$req" | jq -r .id); code=$(echo "$req" | jq -r .code); name=$(echo "$req" | jq -r .name)
vcode=$(sed -n 's/^PAIRING code=\([0-9]*\) .*/\1/p' "$art/viewerA.log" | head -1)
[ "$code" = "$vcode" ] || fail "the code on the GUI ($code) differs from the device's ($vcode)"
[ "$name" = pdisp-view ] || fail "the device name was '$name'"
C answer "$id" deny >/dev/null || fail "answer deny failed"
wait "$vpid"; vrc=$?; vpid=
[ "$vrc" = 5 ] || fail "a denied device was not refused (viewer exit $vrc)"
C answer "$id" deny >/dev/null 2>&1 && fail "a second answer to the same request was accepted"
kill "$cpid" 2>/dev/null; wait "$cpid" 2>/dev/null; cpid=
C devices | jq -e '.devices | length == 0' >/dev/null || fail "a denied device was stored"
echo "ok 3: pairing request reached the GUI with the right code and name; deny refused the device"

# 4. always: the device streams, and the state shows it
C pairing 25 >"$art/pair2.out" 2>&1 &
cpid=$!
sleep 0.5
timeout 60 "$viewer" 127.0.0.1 "$PORT" -i "$art/phoneA" -S 25 >"$art/viewerB.log" 2>&1 &
vpid=$!
i=0; while [ $i -lt 50 ] && ! grep -q '"event":"pairing"' "$art/pair2.out" 2>/dev/null; do sleep 0.2; i=$((i + 1)); done
id=$(grep '"event":"pairing"' "$art/pair2.out" | head -1 | jq -r .id)
C answer "$id" always >/dev/null || fail "answer always failed"
wait_state '.session != null and .session.fps > 0 and .monitor != null' 20 || fail "no streaming session in the state: $(state)"
st=$(state)
echo "$st" | jq -e '.session.device == "pdisp-view" and .session.transport == "network" and .session.width > 0 and .session.refresh_hz > 0 and (.monitor.connector | length) > 0 and .monitor.grace == false' >/dev/null || fail "unexpected session state: $st"
grep -q '"result":"always"' "$art/pair2.out" || sleep 1
grep -q '"event":"pairing_done"' "$art/pair2.out" || fail "the GUI was not told the request was decided"
echo "ok 4: always: streaming, state shows $(echo "$st" | jq -c '{device: .session.device, size: "\(.session.width)x\(.session.height)", fps: .session.fps, connector: .monitor.connector}')"

# 5. the paired devices list
C devices | jq -e '.devices | length == 1 and .[0].name == "pdisp-view" and (.[0].fingerprint | length) == 24' >/dev/null || fail "the paired list is wrong: $(C devices)"
echo "ok 5: the paired device is listed"

# 6. disconnect ends the session and removes the monitor at once
C disconnect >/dev/null || fail "disconnect failed"
wait_state '.session == null and .monitor == null' 10 || fail "disconnect did not remove the session and monitor: $(state)"
wait "$vpid" 2>/dev/null; vpid=; kill "$cpid" 2>/dev/null; wait "$cpid" 2>/dev/null; cpid=
echo "ok 6: disconnect removed the session and the monitor"

# 7. revoking
fp=$(C devices | jq -r '.devices[0].fingerprint')
C remove "$(echo "$fp" | cut -c1-9)" | jq -e '.removed == 1' >/dev/null || fail "remove did not remove the device"
C devices | jq -e '.devices | length == 0' >/dev/null || fail "the device is still listed"
C remove ab >/dev/null 2>&1 && fail "a 2-character prefix removed something"
echo "ok 7: a device was revoked"

# 8. settings
C set refresh_cap_hz 90 >/dev/null || fail "set refresh_cap_hz failed"
state | jq -e '.config.refresh_cap_hz == 90' >/dev/null || fail "the new setting is not in the state"
grep -q '^refresh_cap_hz=90$' "$art/pc/pdispd.conf" || fail "the setting was not saved"
C set bitrate_kbps 20000 >/dev/null || fail "set bitrate_kbps failed"
C set refresh_cap_hz 5 >/dev/null 2>&1 && fail "an out-of-range value was accepted"
C set bogus 1 >/dev/null 2>&1 && fail "an unknown setting was accepted"
state | jq -e '.config.refresh_cap_hz == 90 and .config.bitrate_kbps == 20000' >/dev/null || fail "a refused value changed the settings"
echo "ok 8: settings are applied, saved and validated"

# 9. the settings file is read at the next start, and the command line wins over it
kill "$dpid" 2>/dev/null; wait "$dpid" 2>/dev/null; dpid=
sleep 1
"$daemon" -C "$sock" -K "$art/pc" -P deny -p "$PORT" -t PDISPCTL -n pdisp-test -b 8000 >"$art/daemon3.log" 2>&1 &
dpid=$!
i=0; while [ $i -lt 50 ] && ! [ -S "$sock" ]; do sleep 0.2; i=$((i + 1)); done
state | jq -e '.config.refresh_cap_hz == 90 and .config.bitrate_kbps == 8000' >/dev/null || fail "the settings file was not applied, or -b did not win: $(state)"
echo "ok 9: the saved settings are used at the next start, the command line wins"
echo "PASS: control socket: state, pairing, disconnect, devices, settings"
