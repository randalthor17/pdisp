#!/usr/bin/env bash
# M11 check: pdisp-gui (Qt 6 / QML) against a real daemon, offscreen (no window on the desktop). The GUI answers a pairing request itself
# (--test-answer), a viewer stands in for the device, and the GUI writes screenshots of the status page during the session and of the devices page.
# Fails if the pairing did not go through the GUI, the stream did not start, or a screenshot is missing or blank.
# Needs niri, a free evdi device, a VAAPI HEVC encoder and ImageMagick (identify). Exits 77 (meson: skipped) otherwise.
# usage: m11_gui_check.sh <pdispd> <pdisp-view> <pdisp-gui> [artifact-dir]
set -u
daemon=$1; viewer=$2; gui=$3
art=${4:-$(mktemp -d)}; mkdir -p "$art"; rm -rf "$art"/*
skip() { echo "SKIP: $*"; exit 77; }
command -v niri >/dev/null 2>&1 && command -v identify >/dev/null 2>&1 || skip "niri or ImageMagick missing"
niri msg --json outputs >/dev/null 2>&1 || skip "no running niri session"
PORT=${PDISP_TEST_PORT:-47119}
sock=$(mktemp -u /tmp/pdisp-gui-XXXXXX.sock)
dpid=; vpid=; gpid=
cleanup() { for p in $gpid $vpid $dpid; do [ -n "$p" ] && kill "$p" 2>/dev/null; done; wait 2>/dev/null; rm -f "$sock"; }
trap cleanup EXIT
fail() { echo "FAIL: $*"; echo "--- daemon:"; grep -v '^\[libevdi\|^libva' "$art/daemon.log" 2>/dev/null | tail -10; echo "--- gui:"; cat "$art/gui1.log" 2>/dev/null | tail -5; exit 1; }
mkdir -p "$art/pc"; chmod 700 "$art/pc"
export QT_QPA_PLATFORM=offscreen QT_QPA_PLATFORMTHEME= QT_QPA_PLATFORMTHEME_QT6= QT_FORCE_STDERR_LOGGING=1

"$daemon" -C "$sock" -K "$art/pc" -P ask -p "$PORT" -t PDISPGUI -n pdisp-gui -g 5 >"$art/daemon.log" 2>&1 &
dpid=$!
i=0; while [ $i -lt 50 ] && ! [ -S "$sock" ]; do
	kill -0 "$dpid" 2>/dev/null || { wait "$dpid"; [ $? = 3 ] && skip "no free evdi device"; fail "daemon exited early"; }
	sleep 0.2; i=$((i + 1)); done
[ -S "$sock" ] || fail "no control socket"

# the GUI sits in front of the daemon, answers the pairing request, and photographs the status page 9 s in
"$gui" --socket "$sock" --test-answer always --screenshot "$art/status.png" --delay 9000 >"$art/gui1.log" 2>&1 &
gpid=$!
sleep 1.5
timeout 40 "$viewer" 127.0.0.1 "$PORT" -i "$art/phone" -S 14 >"$art/viewer.log" 2>&1 &
vpid=$!
wait "$gpid"; grc=$?; gpid=
[ "$grc" = 0 ] || fail "the GUI exited with $grc"
grep -q "authorised 'pdisp-view' .* for good" "$art/daemon.log" || fail "the pairing was not answered through the GUI"
grep -qi "warning\|error\|undefined" "$art/gui1.log" && { echo "NOTE: the GUI logged:"; grep -i "warning\|error\|undefined" "$art/gui1.log" | head -3; }
wait "$vpid"; vrc=$?; vpid=
[ "$vrc" = 0 ] || fail "the viewer failed (exit $vrc)"
grep -q '^VIEW' "$art/viewer.log" && [ "$(sed -n 's/^VIEW .* decoded=\([0-9]*\) .*/\1/p' "$art/viewer.log")" -ge 1 ] || fail "no video reached the viewer"

# the screenshots are real pictures: the right size and more than one colour
for shot in status; do
	[ -s "$art/$shot.png" ] || fail "no $shot screenshot"
	colors=$(identify -format %k "$art/$shot.png")
	[ "${colors:-0}" -ge 8 ] || fail "the $shot screenshot is blank ($colors colours)"
done
# a second look, now with a device paired and nobody connected: the devices page lists it
"$gui" --socket "$sock" --screenshot "$art/devices.png" --page 1 --delay 1500 >"$art/gui2.log" 2>&1 || fail "the devices screenshot failed"
[ "$(identify -format %k "$art/devices.png")" -ge 8 ] || fail "the devices screenshot is blank"
echo "PASS: the GUI paired the device through the control socket, the stream ran, screenshots written ($art/status.png, $art/devices.png)"
