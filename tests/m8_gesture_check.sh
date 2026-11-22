#!/usr/bin/env bash
# M8 check: real touch gestures work through the whole chain. A scripted phone pinches out, drags and pinches in on the virtual output while
# `loupe` (GTK4's image viewer, which turns touch events into zoom and pan) shows a test picture fullscreen there; window screenshots after
# each gesture must differ from the one before by a lot.
#
# Like m8_touch_check.sh this CHANGES THE RUNNING NIRI'S CONFIG for about 30 seconds (a touch map-to-output line, your relative includes made
# absolute, restored by explicit path afterwards and focus given back), so it only runs with PDISP_TEST_NIRI_CONFIG=1. It also opens one
# loupe window on the virtual output only.
# Needs niri, jq, loupe, ImageMagick, a free evdi device, a VAAPI HEVC encoder and write access to /dev/uinput. Exits 77 otherwise.
# usage: m8_gesture_check.sh <pdispd> <pdisp-view> [artifact-dir]
set -u
daemon=$1; viewer=$2
art=${3:-$(mktemp -d)}; mkdir -p "$art"; rm -rf "${art:?}"/*
skip() { echo "SKIP: $*"; exit 77; }
[ -n "${PDISP_TEST_NIRI_CONFIG:-}" ] || skip "this check changes the running niri's config for a moment: set PDISP_TEST_NIRI_CONFIG=1 to allow it"
for c in niri jq loupe magick; do command -v $c >/dev/null 2>&1 || skip "$c is not installed"; done
niri msg --json outputs >/dev/null 2>&1 || skip "no running niri session"
[ -w /dev/uinput ] || skip "no write access to /dev/uinput"

PORT=${PDISP_TEST_PORT:-47118}; SERIAL=PDISPGEST; ID="PNP(PDI) pdisp $SERIAL"
real_file=${PDISP_TEST_NIRI_REAL_CONFIG:-$HOME/.config/niri/config.kdl}; real_dir=$(dirname "$real_file") # what to put back afterwards (the throwaway niri of tests/niri-session.sh has its own)
orig_out=$(niri msg --json focused-output | jq -r '.name'); orig_win=$(niri msg --json focused-window | jq -r '.id // empty')
dpid=; vpid=; lpid=
restore() {
	[ -n "$lpid" ] && kill "$lpid" 2>/dev/null
	for p in $vpid $dpid; do [ -n "$p" ] && kill "$p" 2>/dev/null; done; wait 2>/dev/null; sleep 1
	niri msg action load-config-file --path "$real_file" >/dev/null 2>&1   # the explicit path: without one niri reloads the last loaded file
	[ -n "$orig_out" ] && niri msg action focus-monitor "$orig_out" >/dev/null 2>&1
	[ -n "$orig_win" ] && niri msg action focus-window --id "$orig_win" >/dev/null 2>&1
}
trap restore EXIT
fail() { echo "FAIL: $*"; grep -v '^\[libevdi\|^libva' "$art/daemon.log" | tail -5; tail -4 "$art/viewer.log"; exit 1; }

cfg="$art/niri-touch.kdl"
sed "s|^\(include[^\"]*\)\"\([^/][^\"]*\)\"|\1\"$real_dir/\2\"|" "$real_file" > "$cfg.base"
awk -v v="$ID" '{ print } /^input \{/ && !done { printf "    touch {\n        map-to-output \"%s\"\n    }\n", v; done=1 }' "$cfg.base" > "$cfg"
niri validate --config "$cfg" >/dev/null 2>&1 || skip "cannot build a valid test config from yours"
magick -size 1600x1200 gradient:'#203060-#c0a040' -fill '#e03030' -draw 'rectangle 0,0 400,300' -fill '#30c050' -draw 'rectangle 1200,0 1600,300' \
	-fill '#3080e0' -draw 'rectangle 0,900 400,1200' -fill white -draw 'rectangle 780,0 820,1200' -draw 'rectangle 0,580 1600,620' "$art/test.png"

"$daemon" -p "$PORT" -K "$art/pc" -P accept -t "$SERIAL" -n pdisp -S 90 >"$art/daemon.log" 2>&1 &
dpid=$!
i=0; while [ $i -lt 40 ] && ! grep -q "listening on port" "$art/daemon.log"; do
	kill -0 "$dpid" 2>/dev/null || { wait "$dpid"; [ $? = 3 ] && skip "no free evdi device"; fail "daemon exited early"; }
	sleep 0.25; i=$((i + 1)); done
# pinch out (zoom in), drag up (pan), pinch in (zoom out); each step is reported when sent
"$viewer" 127.0.0.1 "$PORT" -i "$art/phone" -W 2304 -H 1440 -S 70 -J 20 \
	-j "pinch:0.5,0.5:0.04:0.25 hold:3000 swipe:0.5,0.85:0.5,0.25 hold:3000 pinch:0.5,0.5:0.25:0.05 hold:3000" >"$art/viewer.log" 2>&1 &
vpid=$!
out=""; i=0; while [ $i -lt 80 ] && [ -z "$out" ]; do
	out=$(niri msg --json outputs | jq -r --arg s "$SERIAL" 'to_entries[] | select(.value.serial == $s) | .key'); sleep 0.25; i=$((i + 1)); done
[ -n "$out" ] || fail "the virtual output never appeared"
niri msg output "$out" scale 1 >/dev/null 2>&1
niri msg action load-config-file --path "$cfg" >/dev/null 2>&1; sleep 1
niri msg action focus-monitor "$out"; sleep 0.5
loupe "$art/test.png" >"$art/loupe.log" 2>&1 &
lpid=$!
wid=""; i=0; while [ $i -lt 40 ] && [ -z "$wid" ]; do
	wid=$(niri msg --json windows | jq -r '.[] | select(.app_id | test("loupe"; "i")) | .id' | head -1); sleep 0.25; i=$((i + 1)); done
[ -n "$wid" ] || fail "loupe did not open"
niri msg action fullscreen-window --id "$wid"; sleep 4.5 # let it lay the picture out for the fullscreen window
shot() { niri msg action screenshot-window --id "$wid" --path "$art/$1.png" >/dev/null 2>&1; sleep 0.4; }
step_done() { local n=0; while [ $n -lt 240 ]; do grep -q "TOUCHSTEP $1 done" "$art/viewer.log" && return 0; sleep 0.25; n=$((n + 1)); done; fail "gesture step $1 was never sent"; }
shot before
step_done 1; sleep 1.5; shot zoom_in
step_done 2; sleep 1.5; shot pan
step_done 3; sleep 1.5; shot zoom_out

# how much of the window changed (fraction of pixels differing by more than 3%)
changed() { magick compare -metric AE -fuzz 3% "$art/$1.png" "$art/$2.png" null: 2>&1 | sed -n 's/.*(\([0-9.e-]*\)).*/\1/p'; }
check() { awk -v v="$(changed "$1" "$2")" -v m="$3" 'BEGIN { exit !(v >= m) }' || fail "$4: only $(changed "$1" "$2") of the window changed (want at least $3)"; echo "ok: $4 ($(changed "$1" "$2") of the window changed)"; }
check before zoom_in 0.10 "the pinch zoomed the picture in"
check zoom_in pan 0.05 "the drag panned it"
check pan zoom_out 0.05 "the reverse pinch zoomed it out"
sent=$(sed -n 's/^VIEW .* touch_sent=\([0-9]*\) .*/\1/p' "$art/viewer.log")
echo "PASS: pinch zoom, drag pan and pinch zoom-out work in a real application through the whole chain"
