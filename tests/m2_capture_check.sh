#!/bin/sh
# M2 pass check against a running niri session:
#   1. a test pattern drawn on the virtual output round-trips through evdi capture bit-exactly (PNG kept)
#   2. with an animated source, updates arrive at about the output refresh rate, with partial dirty rects,
#      and the grab time per frame is logged
# Needs niri, jq and a free evdi device. Exits 77 (meson: skipped) when those are missing.
# usage: m2_capture_check.sh <pdisp-capture> <pdisp-pattern> [artifact-dir]
set -u
capture=$1
pattern=$2
art=${3:-$(mktemp -d)}
mkdir -p "$art"

skip() { echo "SKIP: $*"; exit 77; }
command -v niri >/dev/null 2>&1 || skip "niri not installed"
command -v jq >/dev/null 2>&1 || skip "jq not installed"
niri msg --json outputs >/dev/null 2>&1 || skip "no running niri session"
[ -n "${WAYLAND_DISPLAY:-}" ] || skip "no WAYLAND_DISPLAY"

SERIAL=PDISPM2CHK
MODE=2304x1440@60
png="$art/m2-roundtrip.png"
cap_log="$art/capture.log"
pat_log="$art/pattern.log"
cap_pid=
pat_pid=

cleanup() {
	[ -n "$pat_pid" ] && kill "$pat_pid" 2>/dev/null
	[ -n "$cap_pid" ] && kill "$cap_pid" 2>/dev/null
	wait 2>/dev/null
}
trap cleanup EXIT

fail() { echo "FAIL: $*"; echo "--- capture log:"; cat "$cap_log" 2>/dev/null; echo "--- pattern log:"; cat "$pat_log" 2>/dev/null; exit 1; }

output_name() {
	niri msg --json outputs 2>/dev/null |
		jq -r --arg s "$SERIAL" 'to_entries | map(select(.value.serial == $s)) | first | .key // empty'
}

# --- part 1: bit-exact round trip of the static pattern
"$capture" -m "$MODE" -s 234x150 -n pdisp-tablet -t "$SERIAL" -N 11 -V -o "$png" -T 30 >"$cap_log" 2>&1 &
cap_pid=$!

name=
i=0
while [ $i -lt 40 ]; do
	name=$(output_name)
	[ -n "$name" ] && break
	if ! kill -0 "$cap_pid" 2>/dev/null; then
		wait "$cap_pid"; rc=$?
		[ "$rc" = 3 ] && skip "no free evdi device"
		fail "capture tool exited early (rc=$rc)"
	fi
	sleep 0.2
	i=$((i + 1))
done
[ -n "$name" ] || fail "output with serial $SERIAL never appeared"
echo "output: $name"

# the pattern is drawn pixel-for-pixel, so the output must be at scale 1
niri msg output "$name" scale 1 >/dev/null 2>&1 || fail "could not set scale 1 on $name"
sleep 0.5

"$pattern" -o "$name" -t 20 >"$pat_log" 2>&1 &
pat_pid=$!

wait "$cap_pid"; rc=$?
cap_pid=
cat "$cap_log" | grep -v '^\[libevdi\]'
[ "$rc" = 0 ] || fail "pattern round trip did not match (capture rc=$rc)"
[ -s "$png" ] || fail "no PNG written"
echo "PNG: $png ($(wc -c <"$png") bytes)"

kill "$pat_pid" 2>/dev/null; wait "$pat_pid" 2>/dev/null; pat_pid=
sleep 2 # let the output settle after the client leaves

# --- part 2: animated source, statistics. Reuse the output: a fresh capture session on the same identity.
"$capture" -m "$MODE" -s 234x150 -n pdisp-tablet -t "$SERIAL" -N 11 -S 6 -T 40 >"$cap_log" 2>&1 &
cap_pid=$!
name=
i=0
while [ $i -lt 40 ] && [ -z "$name" ]; do name=$(output_name); sleep 0.2; i=$((i + 1)); done
[ -n "$name" ] || fail "output did not return for the statistics run"
niri msg output "$name" scale 1 >/dev/null 2>&1
sleep 0.5
"$pattern" -o "$name" -a -t 20 >"$pat_log" 2>&1 &
pat_pid=$!

wait "$cap_pid"; rc=$?
cap_pid=
stats=$(grep '^STATS' "$cap_log")
echo "$stats"
[ "$rc" = 0 ] && [ -n "$stats" ] || fail "statistics run failed (rc=$rc)"

num() { echo "$stats" | tr ' ' '\n' | grep "^$1=" | cut -d= -f2 | tr -d 's%'; }
fps=$(num fps); partial=$(num partial); full=$(num full); grab_max=$(num max)
awk -v f="$fps" 'BEGIN { exit !(f >= 50 && f <= 61) }' || fail "update rate $fps fps is not near the 60 Hz refresh"
[ "$partial" -gt "$full" ] || fail "dirty rects are not mostly partial for an animated bar (partial=$partial full=$full)"
awk -v g="$grab_max" 'BEGIN { exit !(g < 16.7) }' || fail "grab time max ${grab_max} ms exceeds one frame at 60 Hz"

echo "PASS: bit-exact round trip; animated capture $fps fps, partial=$partial full=$full, max grab ${grab_max} ms"
