#!/usr/bin/env bash
# M14 check: the video codec is negotiated. The PC streams HEVC when the phone can decode it, H.264 when that is all it can (or all the PC allows),
# and refuses a phone that has none of the codecs the PC allows. Expected:
#   1. a phone that offers both gets HEVC
#   2. a phone that offers only H.264 gets H.264
#   3. a PC set to H.264 only (-c h264) gives H.264 even to a phone that offers both
#   4. a PC set to HEVC only (-c hevc) refuses a phone that offers only H.264, saying why (and creates no virtual output)
#   5. the setting is changed and read back over the control socket (pdisp set codecs), and a wrong value is refused
# Needs an evdi device and a VAAPI H.264 and HEVC encoder. Exits 77 (skipped) otherwise. It makes no output in your compositor beyond the virtual
# one (three short sessions), and needs no niri.
# usage: m14_codec_check.sh <pdispd> <pdisp-view> <pdisp> [artifact-dir]
set -u
daemon=$1; viewer=$2; ctl=$3
art=${4:-$(mktemp -d)}; mkdir -p "$art"; rm -rf "${art:?}"/*
skip() { echo "SKIP: $*"; exit 77; }
PORT=${PDISP_TEST_PORT:-24124}; SERIAL=PDISPM14CHK
sock="$art/control.sock"
dpid=
stop_daemon() { [ -n "$dpid" ] && { kill "$dpid" 2>/dev/null; wait "$dpid" 2>/dev/null; }; dpid=; }
cleanup() { stop_daemon; wait 2>/dev/null; }
trap cleanup EXIT
fail() { echo "FAIL: $*"; echo "--- daemon:"; grep -v '^\[libevdi\]\|^libva' "$art/daemon.log" 2>/dev/null | tail -15; echo "--- viewer:"; tail -6 "$art/viewer.log" 2>/dev/null; exit 1; }

start_daemon() { # extra daemon arguments
	"$daemon" -K "$art/pc" -C "$sock" -P accept -p "$PORT" -t "$SERIAL" -n pdisp -g 0 "$@" >"$art/daemon.log" 2>&1 &
	dpid=$!
	sleep 0.7
	if ! kill -0 "$dpid" 2>/dev/null; then wait "$dpid"; [ $? = 3 ] && skip "no free evdi device"; fail "daemon exited early"; fi
}
# runs a phone offering the given codecs for 2.5 s (nothing draws on the virtual output, so no picture is sent and the viewer would wait for one);
# the viewer's output is in viewer.log, its exit status in $rc. That the streams decode is checked by the m4 loopback test with -F h264.
phone() { # offered codecs
	timeout -s INT 2.5 "$viewer" 127.0.0.1 "$PORT" -i "$art/phone" -W 1280 -H 800 -S 3 -F "$1" >"$art/viewer.log" 2>&1
	rc=$?
}
setup_codec() { sed -n 's/.*SETUP: \([a-z0-9]*\),.*/\1/p' "$art/viewer.log" | head -1; }

echo "-- 1. a phone that offers both codecs gets HEVC"
start_daemon
phone hevc,h264
grep -q "no free evdi\|cannot open a hevc encoder" "$art/daemon.log" && skip "no VAAPI HEVC encoder or no evdi device"
[ "$(setup_codec)" = hevc ] || fail "expected hevc, got '$(setup_codec)'"
grep -q "encoder: hevc," "$art/daemon.log" || fail "the daemon did not log an hevc encoder"
echo "   ok"

echo "-- 2. a phone that only has H.264 gets H.264"
phone h264
grep -q "cannot open a h264 encoder" "$art/daemon.log" && skip "no VAAPI H.264 encoder"
[ "$(setup_codec)" = h264 ] || fail "expected h264, got '$(setup_codec)'"
grep -q "encoder: h264," "$art/daemon.log" || fail "the daemon did not log an h264 encoder"
echo "   ok"
stop_daemon

echo "-- 3. a PC set to H.264 only streams H.264 to a phone that offers both"
start_daemon -c h264
phone hevc,h264
[ "$(setup_codec)" = h264 ] || fail "expected h264, got '$(setup_codec)'"
echo "   ok"
stop_daemon

echo "-- 4. a PC set to HEVC only refuses a phone that has only H.264"
start_daemon -c hevc
phone h264
{ [ "$rc" != 0 ] && [ "$rc" != 124 ] && [ "$rc" != 130 ]; } || fail "the phone was not refused (exit status $rc)"
grep -q "no video codec in common" "$art/viewer.log" || fail "the refusal did not say why: $(tail -2 "$art/viewer.log")"
grep -q "virtual output" "$art/daemon.log" && fail "a virtual output was created for a refused phone"
echo "   ok: $(grep 'rejected' "$art/viewer.log" | head -1)"
stop_daemon

echo "-- 5. the setting over the control socket"
start_daemon
i=0; while [ $i -lt 20 ] && [ ! -S "$sock" ]; do sleep 0.1; i=$((i + 1)); done
[ -S "$sock" ] || fail "no control socket"
"$ctl" -s "$sock" status 2>/dev/null | grep -q '"codecs":"hevc,h264"' || fail "status does not show the default codecs: $("$ctl" -s "$sock" status 2>&1 | head -3)"
"$ctl" -s "$sock" set codecs h264 >"$art/set.log" 2>&1 || fail "pdisp set codecs h264 failed: $(cat "$art/set.log")"
sleep 0.3
"$ctl" -s "$sock" status 2>/dev/null | grep -q '"codecs":"h264"' || fail "status does not show codecs=h264 after the change"
grep -q "^codecs=h264" "$art/pc/pdispd.conf" || fail "the setting was not saved: $(cat "$art/pc/pdispd.conf" 2>&1)"
"$ctl" -s "$sock" set codecs vp9 >"$art/bad.log" 2>&1 && fail "a codec the PC cannot encode was accepted"
"$ctl" -s "$sock" status 2>/dev/null | grep -q '"codecs":"h264"' || fail "a refused value changed the setting"
stop_daemon
start_daemon # the saved setting applies at the next start
i=0; while [ $i -lt 20 ] && [ ! -S "$sock" ]; do sleep 0.1; i=$((i + 1)); done
"$ctl" -s "$sock" status 2>/dev/null | grep -q '"codecs":"h264"' || fail "the saved codecs setting was not read at start"
echo "   ok"

echo "PASS: the codec is negotiated (HEVC preferred, H.264 when needed or required), refused when there is none in common, and the setting persists"
