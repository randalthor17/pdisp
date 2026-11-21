#!/usr/bin/env bash
# M7 check: the monitor reaches the tablet over the USB cable (AOA), with the tablet showing the stream smoothly.
# The stream itself uses no adb; adb is only used to read the app's statistics, install the build and make the tablet leave
# accessory mode so the plug-in sequence runs again. Needs: niri, jq, a free evdi device, a tablet with the pdisp app and adb
# authorised. Exits 77 (meson: skipped) when they are missing.
# usage: m7_usb_check.sh <pdispd> <pdisp-pattern> [artifact-dir]
set -u
daemon=$1; pattern=$2
art=${3:-$(mktemp -d)}; mkdir -p "$art"
root=$(cd "$(dirname "$0")/.." && pwd)
skip() { echo "SKIP: $*"; exit 77; }
[ -z "${PDISP_NO_DEVICE:-}" ] || skip "PDISP_NO_DEVICE is set"
adb() { if type -P adb >/dev/null 2>&1; then "$(type -P adb)" "$@"; else nix develop "$root#android" -c adb "$@" 2>/dev/null; fi; }

command -v niri >/dev/null 2>&1 && command -v jq >/dev/null 2>&1 || skip "niri or jq missing"
niri msg --json outputs >/dev/null 2>&1 || skip "no running niri session"
adb devices | grep -q "	device" || skip "no authorised adb device"
adb shell pm list packages 2>/dev/null | grep -q "dev.pdisp" || skip "the pdisp app is not installed on the tablet"

dlog="$art/daemon.log"
dpid=; ppid=
cleanup() { for p in $ppid $dpid; do [ -n "$p" ] && kill "$p" 2>/dev/null; done; wait 2>/dev/null; }
trap cleanup EXIT
fail() { echo "FAIL: $*"; grep -v '^\[libevdi\|^libva' "$dlog" | tail -12; exit 1; }

adb shell am force-stop dev.pdisp; adb shell input keyevent KEYCODE_HOME
adb shell svc usb setFunctions mtp >/dev/null; sleep 4 # leave accessory mode: the daemon must switch the device again
# Android asks "Open pdisp to handle pdisp second monitor?" on a plug-in (some phones every time): answer it without ticking "Always"
tap_prompt() {
	adb shell uiautomator dump /sdcard/pdisp_ui.xml >/dev/null 2>&1
	local xml; xml=$(adb shell cat /sdcard/pdisp_ui.xml 2>/dev/null)
	echo "$xml" | grep -q "pdisp second monitor" || return 1
	local label c
	for label in "Only this time" "OK"; do
		c=$(echo "$xml" | tr '>' '\n' | grep "text=\"$label\"" | grep -o 'bounds="\[[0-9]*,[0-9]*\]\[[0-9]*,[0-9]*\]"' | head -1 | tr -cs '0-9' ' ' | awk '{ printf "%d %d", ($1 + $3) / 2, ($2 + $4) / 2 }')
		[ -n "$c" ] && { adb shell input tap $c; return 0; }
	done
	return 1
}

"$daemon" -u -P accept -K "$art/pc-id" >"$dlog" 2>&1 &
dpid=$!
i=0; while [ $i -lt 120 ] && ! grep -q "streaming to" "$dlog"; do
	[ $((i % 6)) = 4 ] && tap_prompt
	kill -0 "$dpid" 2>/dev/null || { wait "$dpid"; [ $? = 3 ] && skip "no free evdi device"; fail "daemon exited early"; }
	sleep 0.5; i=$((i + 1)); done
grep -q "USB accessory" "$dlog" || fail "the daemon never found the tablet on USB"
grep -q "streaming to" "$dlog" || { adb exec-out screencap -p >"$art/tablet.png"; fail "no stream over USB (see $art/tablet.png: an unanswered prompt?)"; }

name=$(niri msg --json outputs | jq -r 'to_entries[] | select(.value.serial=="PDISP0001") | .key')
[ -n "$name" ] || fail "the virtual output is not there"
niri msg output "$name" scale 1 >/dev/null 2>&1
"$pattern" -o "$name" -a -T -t 40 >"$art/pattern.log" 2>&1 &
ppid=$!
sleep 8; adb logcat -c; sleep 24
adb logcat -d -s pdisp:* | grep -E "CADENCE|ARRIVAL|DECODE" | cut -c30- >"$art/stats.txt"
adb exec-out screencap -p >"$art/tablet.png"
cat "$art/stats.txt" | tail -9

# the last three one-second-window reports: smooth presentation, nothing late
cad=$(grep CADENCE "$art/stats.txt" | tail -3)
[ "$(echo "$cad" | wc -l)" = 3 ] || fail "no cadence statistics from the app"
echo "$cad" | while read -r line; do
	p95=$(echo "$line" | sed -n 's/.*p95=\([0-9.]*\).*/\1/p'); late=$(echo "$line" | sed -n 's/.*over25=\([0-9]*\).*/\1/p')
	awk -v p="$p95" -v l="$late" 'BEGIN { exit !(p <= 20 && l <= 9) }' || { echo "FAIL: cadence p95 ${p95} ms, ${late} frames over 25 ms"; exit 1; }
done || exit 1
lost=$(grep -E "frames [0-9]+ " "$dlog" | tail -1 | sed -n 's/.*lost \([0-9]*\).*/\1/p')
[ "${lost:-99}" -le 30 ] || fail "$lost frames lost on a reliable cable"
adb shell svc usb setFunctions mtp >/dev/null; sleep 5
grep -q "no packets from the phone\|unplugged\|control connection closed" "$dlog" || fail "the daemon did not notice the tablet leaving"
echo "PASS: the monitor streams to the tablet over USB, cadence smooth in the last 3 windows, $lost frames lost, the daemon noticed the unplug"
