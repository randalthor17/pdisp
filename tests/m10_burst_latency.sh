#!/bin/sh
# M10: what static refinement costs a change that arrives right after it. The text page stays static and only a pixel timestamp changes
# once a second (pdisp-pattern -X -T -B 1), so each change lands 0.15 s to 1.3 s after the previous one, inside or after a refinement burst.
# pdisp-view -G reports glass-to-glass latency of the first picture carrying each stamp (refinement repeats of a stamp are not counted).
# Run it with DAEMON_ARGS="-F 0" for the baseline. Prints the GLASS line; fails when p90 exceeds GLASS_MAX_MS (default 50). p99 is not used: the first two or three stamps of a run (the opening keyframe of this page is about 1 MB) take 400 to 800 ms with or without refinement, and a 20-second run has too few stamps to leave them out of p99.
# usage: m10_burst_latency.sh <pdispd> <pdisp-view> <pdisp-pattern> [artifact-dir]
set -u
daemon=$1; viewer=$2; pattern=$3
art=${4:-$(mktemp -d)}; mkdir -p "$art"
skip() { echo "SKIP: $*"; exit 77; }
command -v niri >/dev/null 2>&1 && command -v jq >/dev/null 2>&1 || skip "niri or jq missing"
niri msg --json outputs >/dev/null 2>&1 || skip "no running niri session"
SERIAL=PDISPM10BST; PORT=${PDISP_TEST_PORT:-47114}; SECS=${SECS:-20}; GAPS=${GAP:-0.4}
dlog="$art/daemon.log"; vlog="$art/viewer.log"; plog="$art/pattern.log"
dpid=; vpid=; ppid=
cleanup() { for p in $ppid $vpid $dpid; do kill "$p" 2>/dev/null; done; wait 2>/dev/null; }
trap cleanup EXIT
fail() { echo "FAIL: $*"; grep -v '^\[libevdi\]\|^libva' "$dlog" | tail -8; tail -5 "$vlog"; exit 1; }
# shellcheck disable=SC2086
"$daemon" -K "$art/pc-id" -P accept -p "$PORT" -t "$SERIAL" -n pdisp-tablet -S $((SECS + 12)) ${DAEMON_ARGS:-} >"$dlog" 2>&1 &
dpid=$!
sleep 0.5
if ! kill -0 "$dpid" 2>/dev/null; then wait "$dpid"; rc=$?; [ "$rc" = 3 ] && skip "no free evdi device"; fail "daemon exited early (rc=$rc)"; fi
"$viewer" 127.0.0.1 "$PORT" -i "$art/phone-id" -W 2304 -H 1440 -r 60 -S "$SECS" -G -o "$art/view.png" >"$vlog" 2>&1 &
vpid=$!
name=; i=0
while [ $i -lt 60 ]; do
	name=$(niri msg --json outputs 2>/dev/null | jq -r --arg s "$SERIAL" 'to_entries | map(select(.value.serial == $s)) | first | .key // empty')
	[ -n "$name" ] && break; sleep 0.2; i=$((i + 1))
done
[ -n "$name" ] || fail "the virtual output never appeared"
niri msg output "$name" scale 1 >/dev/null 2>&1 || fail "could not set scale 1"
sleep 0.5
"$pattern" -o "$name" -X -T -B "$GAPS" -t $((SECS + 6)) >"$plog" 2>&1 &
ppid=$!
wait "$vpid"; vpid=
kill "$ppid" 2>/dev/null; wait "$ppid" 2>/dev/null; ppid=
wait "$dpid" 2>/dev/null; dpid=
grep '^GLASS' "$vlog"
grep '^DAEMON' "$dlog" | cut -c1-140
p90=$(grep '^GLASS' "$vlog" | sed -n 's/.*p90=\([0-9.]*\).*/\1/p')
[ -n "$p90" ] || fail "no glass samples"
awk -v g="$p90" -v m="${GLASS_MAX_MS:-50}" 'BEGIN { exit !(g < m) }' || fail "glass-to-glass p90 ${p90} ms is over ${GLASS_MAX_MS:-50} ms"
echo PASS
