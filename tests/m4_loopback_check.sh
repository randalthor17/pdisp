#!/bin/sh
# M4 loopback check through the real pipeline: niri output (evdi) -> capture -> VAAPI HEVC -> pdispd -> UDP -> pdisp-view ->
# libavcodec. The animated test pattern is shown on the virtual output; the viewer must decode it with the loss handling
# in the loop, match the pattern, and keep up with the refresh rate.
# Needs niri, jq, a free evdi device, a VAAPI HEVC encoder. Exits 77 (meson: skipped) when they are missing.
# usage: m4_loopback_check.sh <pdispd> <pdisp-view> <pdisp-pattern> [artifact-dir] [extra pdisp-view args...]
set -u
daemon=$1; viewer=$2; pattern=$3
art=${4:-$(mktemp -d)}
[ $# -ge 4 ] && shift 4 || shift $#
mkdir -p "$art"

skip() { echo "SKIP: $*"; exit 77; }
command -v niri >/dev/null 2>&1 || skip "niri not installed"
command -v jq >/dev/null 2>&1 || skip "jq not installed"
niri msg --json outputs >/dev/null 2>&1 || skip "no running niri session"
[ -n "${WAYLAND_DISPLAY:-}" ] || skip "no WAYLAND_DISPLAY"

SERIAL=PDISPM4LOOP
PORT=${PDISP_TEST_PORT:-47110}
dlog="$art/daemon.log"; vlog="$art/viewer.log"; plog="$art/pattern.log"
dpid=; vpid=; ppid=

cleanup() {
	for p in $ppid $vpid $dpid; do kill "$p" 2>/dev/null; done
	wait 2>/dev/null
}
trap cleanup EXIT
fail() { echo "FAIL: $*"; echo "--- daemon:"; grep -v '^\[libevdi\]\|^libva' "$dlog" | tail -15; echo "--- viewer:"; tail -8 "$vlog"; exit 1; }

"$daemon" -p "$PORT" -t "$SERIAL" -n pdisp-tablet -S 40 >"$dlog" 2>&1 &
dpid=$!
sleep 0.5
if ! kill -0 "$dpid" 2>/dev/null; then wait "$dpid"; rc=$?; [ "$rc" = 3 ] && skip "no free evdi device"; fail "daemon exited early (rc=$rc)"; fi

"$viewer" 127.0.0.1 "$PORT" -W 2304 -H 1440 -r 60 -S 10 -c -o "$art/view.png" "$@" >"$vlog" 2>&1 &
vpid=$!

name=
i=0
while [ $i -lt 60 ]; do
	name=$(niri msg --json outputs 2>/dev/null | jq -r --arg s "$SERIAL" 'to_entries | map(select(.value.serial == $s)) | first | .key // empty')
	[ -n "$name" ] && break
	sleep 0.2
	i=$((i + 1))
done
[ -n "$name" ] || fail "the virtual output never appeared"
niri msg output "$name" scale 1 >/dev/null 2>&1 || fail "could not set scale 1"
sleep 0.5
"$pattern" -o "$name" -a -t 30 >"$plog" 2>&1 &
ppid=$!

wait "$vpid"; vrc=$?
vpid=
kill "$ppid" 2>/dev/null; wait "$ppid" 2>/dev/null; ppid=
wait "$dpid"; drc=$?
dpid=
summary=$(grep '^VIEW' "$vlog")
echo "$summary"
grep '^DAEMON' "$dlog"
[ "$vrc" = 0 ] || fail "viewer failed (rc=$vrc)"
[ "$drc" = 0 ] || fail "daemon failed (rc=$drc)"

val() { echo "$summary" | tr ' ' '\n' | grep "^$1=" | head -1 | cut -d= -f2 | tr -d '%'; }
decoded=$(val decoded); fps=$(val fps); p99=$(val p99); given=$(val given_up); idr=$(val idr); match=$(val match); errs=$(val decoder_errors)
[ "$decoded" -ge 450 ] 2>/dev/null || fail "decoded only $decoded frames in 10 s"
awk -v f="$fps" 'BEGIN { exit !(f >= 50) }' || fail "decode rate $fps fps is below 50"
awk -v l="$p99" 'BEGIN { exit !(l < 80) }' || fail "capture-to-decode latency p99 ${p99} ms is over 80 ms"
[ "$errs" = 0 ] || fail "$errs decoder errors"
[ "$idr" = 1 ] || fail "$idr IDRs decoded (expected exactly the first)"
[ -s "$art/view.png" ] || fail "no snapshot written"
echo "PASS: $decoded frames at $fps fps, p99 latency ${p99} ms, pattern match ${match}%, given up $given, 1 IDR"
