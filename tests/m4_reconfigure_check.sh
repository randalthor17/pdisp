#!/bin/sh
# M4 check of RECONFIGURE and control-channel liveness through the real pipeline: the viewer reports a display change (a
# rotation to portrait) mid-stream; the daemon rebuilds the virtual output, encoder and sender in a new epoch and sends
# RECONFIGURE; the viewer reopens its decoder and confirms; the stream resumes at the new size and the picture is right.
# Needs niri, jq, a free evdi device, a VAAPI HEVC encoder. Exits 77 (meson: skipped) when they are missing.
# usage: m4_reconfigure_check.sh <pdispd> <pdisp-view> <pdisp-pattern> [artifact-dir]
set -u
daemon=$1; viewer=$2; pattern=$3
art=${4:-$(mktemp -d)}
mkdir -p "$art"

skip() { echo "SKIP: $*"; exit 77; }
command -v niri >/dev/null 2>&1 || skip "niri not installed"
command -v jq >/dev/null 2>&1 || skip "jq not installed"
niri msg --json outputs >/dev/null 2>&1 || skip "no running niri session"
[ -n "${WAYLAND_DISPLAY:-}" ] || skip "no WAYLAND_DISPLAY"

SERIAL=PDISPM4ROT
PORT=${PDISP_TEST_PORT:-24111}
dlog="$art/daemon.log"; vlog="$art/viewer.log"
dpid=; vpid=; p1=; p2=
cleanup() {
	for p in $p2 $p1 $vpid $dpid; do [ -n "$p" ] && kill "$p" 2>/dev/null; done
	wait 2>/dev/null
}
trap cleanup EXIT
fail() { echo "FAIL: $*"; echo "--- daemon:"; grep -v '^\[libevdi\]\|^libva' "$dlog" | tail -15; echo "--- viewer:"; tail -10 "$vlog"; exit 1; }

output_info() { # prints "name width" of the virtual output, if present
	niri msg --json outputs 2>/dev/null | jq -r --arg s "$SERIAL" 'to_entries | map(select(.value.serial == $s)) | first
		| select(. != null) | "\(.key) \(.value.modes[.value.current_mode].width // 0)"'
}

"$daemon" -K "$art/pc-id" -P accept -p "$PORT" -t "$SERIAL" -n pdisp-tablet -S 60 >"$dlog" 2>&1 &
dpid=$!
sleep 0.5
if ! kill -0 "$dpid" 2>/dev/null; then wait "$dpid"; rc=$?; [ "$rc" = 3 ] && skip "no free evdi device"; fail "daemon exited early (rc=$rc)"; fi

# the viewer rotates its display 5 s after the stream starts; the first picture is 2304x1440, the second 1440x2304
"$viewer" 127.0.0.1 "$PORT" -i "$art/phone-id" -W 2304 -H 1440 -r 60 -S 17 -X 5:1440x2304 -c -G -o "$art/view.png" >"$vlog" 2>&1 &
vpid=$!

i=0; info=
while [ $i -lt 60 ]; do info=$(output_info); [ -n "$info" ] && break; sleep 0.2; i=$((i + 1)); done
[ -n "$info" ] || fail "the virtual output never appeared"
name=${info% *}
niri msg output "$name" scale 1 >/dev/null 2>&1 || fail "could not set scale 1"
sleep 0.5
# the first pattern ends before the rotation: a fullscreen window must not be left on an output that disappears
"$pattern" -o "$name" -a -T -t 4 >"$art/pattern1.log" 2>&1 &
p1=$!

# wait for the output to come back in portrait, then show the pattern there
i=0
while [ $i -lt 150 ]; do
	info=$(output_info)
	[ -n "$info" ] && [ "${info#* }" = 1440 ] && break
	sleep 0.2; i=$((i + 1))
done
[ "${info#* }" = 1440 ] || fail "the output never came back at 1440x2304 (reconfigure did not happen)"
name=${info% *}
niri msg output "$name" scale 1 >/dev/null 2>&1 || fail "could not set scale 1 after the rotation"
sleep 0.5
"$pattern" -o "$name" -a -T -t 12 >"$art/pattern2.log" 2>&1 &
p2=$!

wait "$vpid"; vrc=$?
vpid=
kill "$p2" 2>/dev/null; wait "$p2" 2>/dev/null; p2=
wait "$dpid"; drc=$?
dpid=
summary=$(grep '^VIEW' "$vlog")
echo "$summary"
grep -E '^(GLASS|DAEMON)' "$vlog" "$dlog" | sed 's/^[^:]*://'
[ "$vrc" = 0 ] || fail "viewer failed (rc=$vrc)"
[ "$drc" = 0 ] || fail "daemon failed (rc=$drc)"

val() { echo "$summary" | tr ' ' '\n' | grep "^$1=" | head -1 | cut -d= -f2 | tr -d '%'; }
[ "$(val reconfigs)" = 1 ] || fail "viewer saw $(val reconfigs) reconfigurations (expected 1)"
[ "$(val final_size)" = 1440x2304 ] || fail "the last decoded picture is $(val final_size), not 1440x2304"
[ "$(val decoded)" -ge 300 ] 2>/dev/null || fail "decoded only $(val decoded) frames"
[ "$(val decoder_errors)" = 0 ] || fail "$(val decoder_errors) decoder errors"
[ "$(val pongs)" -ge 3 ] 2>/dev/null || fail "only $(val pongs) PONGs: the control channel liveness does not work"
grep -q 'reconfigs=1' "$dlog" || grep -q '^DAEMON.*reconfigs=1' "$dlog" || fail "the daemon did not count one reconfiguration"
awk -v f="$(val freeze_ms)" 'BEGIN { exit !(f < 4000) }' || fail "the stream was down for $(val freeze_ms) ms across the reconfiguration"
echo "PASS: reconfigured to $(val final_size) mid-stream; $(val decoded) frames decoded, longest interruption $(val freeze_ms) ms, picture matches ($(val match)%), $(val pongs) PONGs"
