#!/bin/sh
# M10 text quality: a static text-like page (pdisp-pattern -X) on the virtual output goes through the real pipeline (capture, VAAPI HEVC,
# pdispd, UDP, pdisp-view, libavcodec) and the viewer (-Q) reports every 250 ms how close the decoded picture is to the exact page (PSNR and mean
# absolute error over R, G, B). The page is hashed glyph noise, denser than real text, so the numbers are a lower bound. The run shows how
# quality develops once the screen stops changing, which is what static refinement is about.
# Needs niri, jq, a free evdi device, a VAAPI HEVC encoder. Exits 77 (meson: skipped) when they are missing.
# usage: m10_text_check.sh <pdispd> <pdisp-view> <pdisp-pattern> [artifact-dir]
# Environment: DAEMON_ARGS (extra pdispd options, e.g. "-b 20000"), MIN_PSNR (fail below this at the end; default 27; the page is brutal: 13.5 dB without refinement (-F 0), 30.4 dB with it), SECS (run length, 12).
set -u
daemon=$1; viewer=$2; pattern=$3
art=${4:-$(mktemp -d)}
mkdir -p "$art"
skip() { echo "SKIP: $*"; exit 77; }
command -v niri >/dev/null 2>&1 && command -v jq >/dev/null 2>&1 || skip "niri or jq missing"
niri msg --json outputs >/dev/null 2>&1 || skip "no running niri session"
SERIAL=PDISPM10TXT; PORT=${PDISP_TEST_PORT:-47113}; SECS=${SECS:-12}
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
"$viewer" 127.0.0.1 "$PORT" -i "$art/phone-id" -W 2304 -H 1440 -r 60 -S "$SECS" -Q -o "$art/view.png" >"$vlog" 2>&1 &
vpid=$!
name=; i=0
while [ $i -lt 60 ]; do
	name=$(niri msg --json outputs 2>/dev/null | jq -r --arg s "$SERIAL" 'to_entries | map(select(.value.serial == $s)) | first | .key // empty')
	[ -n "$name" ] && break; sleep 0.2; i=$((i + 1))
done
[ -n "$name" ] || fail "the virtual output never appeared"
niri msg output "$name" scale 1 >/dev/null 2>&1 || fail "could not set scale 1"
sleep 0.5
"$pattern" -o "$name" -X -t $((SECS + 6)) >"$plog" 2>&1 &
ppid=$!
wait "$vpid"; vpid=
kill "$ppid" 2>/dev/null; wait "$ppid" 2>/dev/null; ppid=
wait "$dpid" 2>/dev/null; dpid=

grep TEXTQ "$vlog" | sed 's/.*TEXTQ t=\([0-9.]*\) s psnr=\([0-9.]*\) dB.*/\1:\2/' | tr '\n' ' ' | fold -w 100 | sed 's/^/  /'; echo
last=$(grep TEXTQ "$vlog" | tail -1 | sed 's/.*psnr=\([0-9.]*\) dB.*/\1/')
first=$(grep TEXTQ "$vlog" | sed -n 3p | sed 's/.*psnr=\([0-9.]*\) dB.*/\1/')
[ -n "$last" ] || fail "no quality samples (is the text page on the virtual output?)"
echo "text page PSNR: $first dB early, $last dB at the end"
grep '^DAEMON' "$dlog" | cut -c1-120
awk -v l="$last" -v m="${MIN_PSNR:-27}" 'BEGIN { exit !(l + 0 >= m + 0) }' || fail "the final PSNR $last dB is below $MIN_PSNR dB"
echo "PASS"
