#!/bin/sh
# M10 real text: a terminal showing source code, fullscreen on the virtual output, goes through the real pipeline; the viewer's decoded snapshot is compared
# with the screen as the daemon captured it (PDISP_DUMP_FRAME: exactly what the encoder was given): PSNR over the whole picture.
# Opens a kitty window on the desktop for about 15 seconds and moves it to the virtual output at once, so it is opt-in: PDISP_TEST_WINDOWS=1.
# Needs niri, jq, kitty, ffmpeg, a free evdi device, a VAAPI HEVC encoder. Exits 77 (meson: skipped) otherwise.
# usage: m10_real_text_check.sh <pdispd> <pdisp-view> <pdisp-pattern> [artifact-dir]     env: DAEMON_ARGS, MIN_PSNR (default 30), SECS (14), TEXT_FILE
set -u
daemon=$1; viewer=$2; pattern=$3
art=${4:-$(mktemp -d)}; mkdir -p "$art"
root=$(cd "$(dirname "$0")/.." && pwd)
skip() { echo "SKIP: $*"; exit 77; }
[ "${PDISP_TEST_WINDOWS:-}" = 1 ] || skip "opens a window on the desktop: set PDISP_TEST_WINDOWS=1"
for c in niri jq kitty ffmpeg; do command -v "$c" >/dev/null 2>&1 || skip "$c missing"; done
niri msg --json outputs >/dev/null 2>&1 || skip "no running niri session"
SERIAL=PDISPM10REAL; PORT=${PDISP_TEST_PORT:-24115}; SECS=${SECS:-14}; TEXT_FILE=${TEXT_FILE:-$root/daemon/pdispd.c}
dlog="$art/daemon.log"; vlog="$art/viewer.log"
dpid=; vpid=; kpid=
cleanup() { for p in $kpid $vpid $dpid; do [ -n "$p" ] && kill "$p" 2>/dev/null; done; wait 2>/dev/null; }
trap cleanup EXIT
fail() { echo "FAIL: $*"; grep -v '^\[libevdi\]\|^libva' "$dlog" | tail -6; tail -4 "$vlog"; exit 1; }
# shellcheck disable=SC2086
PDISP_DUMP_FRAME="$art/ref.ppm" "$daemon" -K "$art/pc-id" -P accept -p "$PORT" -t "$SERIAL" -n pdisp-tablet -S $((SECS + 12)) ${DAEMON_ARGS:-} >"$dlog" 2>&1 &
dpid=$!
sleep 0.5
if ! kill -0 "$dpid" 2>/dev/null; then wait "$dpid"; rc=$?; [ "$rc" = 3 ] && skip "no free evdi device"; fail "daemon exited early (rc=$rc)"; fi
"$viewer" 127.0.0.1 "$PORT" -i "$art/phone-id" -W 2304 -H 1440 -r 60 -S "$SECS" -o "$art/view.png" >"$vlog" 2>&1 &
vpid=$!
name=; i=0
while [ $i -lt 60 ]; do
	name=$(niri msg --json outputs 2>/dev/null | jq -r --arg s "$SERIAL" 'to_entries | map(select(.value.serial == $s)) | first | .key // empty')
	[ -n "$name" ] && break; sleep 0.2; i=$((i + 1))
done
[ -n "$name" ] || fail "the virtual output never appeared"
niri msg output "$name" scale 1 >/dev/null 2>&1 || fail "could not set scale 1"
sleep 1
kitty --class pdisp-textcheck --title pdisp-textcheck -o font_size=11 -o cursor_blink_interval=0 -o confirm_os_window_close=0 sh -c "cat '$TEXT_FILE'; sleep 40" >/dev/null 2>&1 &
kpid=$!
id=; i=0
while [ $i -lt 40 ]; do
	id=$(niri msg --json windows 2>/dev/null | jq -r 'map(select(.app_id == "pdisp-textcheck")) | first | .id // empty')
	[ -n "$id" ] && break; sleep 0.1; i=$((i + 1))
done
[ -n "$id" ] || fail "the terminal window never appeared"
niri msg action move-window-to-monitor --id "$id" "$name" >/dev/null 2>&1 || fail "could not move the window"
niri msg action fullscreen-window --id "$id" >/dev/null 2>&1
sleep 8 # the picture settles and refinement runs (the daemon writes the captured screen a second after the last change)
wait "$vpid"; vpid=
kill "$kpid" 2>/dev/null; wait "$kpid" 2>/dev/null; kpid=
wait "$dpid" 2>/dev/null; dpid=
[ -s "$art/ref.ppm" ] && [ -s "$art/view.png" ] || fail "missing the reference or the snapshot"
res() { ffmpeg -hide_banner -i "$art/view.png" -i "$art/ref.ppm" -lavfi "[0:v]format=rgb24[a];[1:v]format=rgb24[b];[a][b]psnr" -f null - 2>&1 | grep -a "average" | sed 's/.*average:\([0-9.inf]*\).*/\1/'; }
sz=$(ffmpeg -hide_banner -i "$art/ref.ppm" 2>&1 | grep -a -o "[0-9]\{3,5\}x[0-9]\{3,5\}" | head -1)
psnr=$(res)
echo "reference size $sz, decoded-vs-screenshot PSNR: $psnr dB"
grep '^DAEMON' "$dlog" | cut -c1-120
[ -n "$psnr" ] || fail "no PSNR (sizes differ? reference $sz)"
awk -v l="$psnr" -v m="${MIN_PSNR:-30}" 'BEGIN { exit !(l + 0 >= m + 0) }' || fail "PSNR $psnr dB is below $MIN_PSNR dB"
echo PASS
