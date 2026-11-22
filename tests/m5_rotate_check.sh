#!/usr/bin/env bash
# M5 check: rotating the phone mid-stream makes the PC rebuild its virtual monitor at the new size (DISPLAY_CHANGED, RECONFIGURE),
# and the picture follows. Uses the Android device attached over adb (Wi-Fi to the PC); needs niri, jq, a free evdi device and the
# pdisp app installed. Exits 77 (meson: skipped) otherwise. Temporarily changes the device's rotation setting and restores it.
# usage: m5_rotate_check.sh <pdispd> <pdisp-pattern> [artifact-dir]   (env: HOSTIP, the PC's address as the device sees it)
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
adb shell pm list packages 2>/dev/null | grep -q "dev.pdisp" || skip "the pdisp app is not installed on the device"
hostip=${HOSTIP:-$(ip -4 route get 1.1.1.1 2>/dev/null | sed -n 's/.* src \([0-9.]*\).*/\1/p')}
[ -n "$hostip" ] || skip "cannot work out the PC's address"

dlog="$art/daemon.log"; dpid=; ppid=
old_acc=$(adb shell settings get system accelerometer_rotation | tr -d '\r'); old_rot=$(adb shell settings get system user_rotation | tr -d '\r')
cleanup() {
	for p in $ppid $dpid; do [ -n "$p" ] && kill "$p" 2>/dev/null; done; wait 2>/dev/null
	adb shell settings put system user_rotation "${old_rot:-0}" >/dev/null; adb shell settings put system accelerometer_rotation "${old_acc:-0}" >/dev/null
	adb shell am force-stop dev.pdisp
}
trap cleanup EXIT
fail() { echo "FAIL: $*"; grep -v '^\[libevdi\|^libva' "$dlog" | tail -10; adb exec-out screencap -p >"$art/device.png"; exit 1; }
# tap the first element whose text is exactly $1 (from the UI dump); succeeds only if there was one
tap_text() { adb shell uiautomator dump /sdcard/pdisp_ui.xml >/dev/null 2>&1
	local c; c=$(adb shell cat /sdcard/pdisp_ui.xml | tr '>' '\n' | grep "text=\"$1\"" | grep -o 'bounds="\[[0-9]*,[0-9]*\]\[[0-9]*,[0-9]*\]"' | head -1 |
		tr -cs '0-9' ' ' | awk '{ printf "%d %d", ($1 + $3) / 2, ($2 + $4) / 2 }')
	[ -n "$c" ] && adb shell input tap $c; }
# centre of a view on the device, from the UI dump: view_center <class>
view_center() { adb shell uiautomator dump /sdcard/pdisp_ui.xml >/dev/null 2>&1
	adb shell cat /sdcard/pdisp_ui.xml | tr '>' '\n' | grep "$1" | grep -o 'bounds="\[[0-9]*,[0-9]*\]\[[0-9]*,[0-9]*\]"' | head -1 |
		tr -cs '0-9' ' ' | awk '{ printf "%d %d", ($1 + $3) / 2, ($2 + $4) / 2 }'; }

"$daemon" -S 120 -n pdisp-dev -P accept -K "$art/pc-id" >"$dlog" 2>&1 &
dpid=$!
i=0; while [ $i -lt 40 ] && ! ss -ltn | grep -q ":47110 "; do kill -0 "$dpid" 2>/dev/null || { wait "$dpid"; [ $? = 3 ] && skip "no free evdi device"; fail "daemon exited"; }; sleep 0.5; i=$((i + 1)); done

adb shell input keyevent KEYCODE_WAKEUP; adb shell wm dismiss-keyguard >/dev/null 2>&1 # a device that went to sleep shows nothing
sleep 1; adb shell dumpsys window | grep -q "mCurrentFocus=.*\(NotificationShade\|Keyguard\)" && skip "the device is locked: unlock it and run the test again"
adb shell pm clear dev.pdisp >/dev/null; adb shell am start -n dev.pdisp/.MainActivity >/dev/null; sleep 3 # fresh app: no saved PC, no pairing
# a cable-attached device starts on a USB attempt: leave it (wait for the app to draw something first)
for i in $(seq 1 20); do tap_text Cancel && break; adb shell cat /sdcard/pdisp_ui.xml | grep -q 'text="Add a PC"' && break; sleep 1; done; sleep 1
adb shell settings put system accelerometer_rotation 0; adb shell settings put system user_rotation 1; sleep 3 # landscape (only works with the app in front)
tap_text "Add a PC" || fail "no Add a PC button on the device"; sleep 1.5
adb shell input text "$hostip"; sleep 1; adb shell input keyevent KEYCODE_ENTER; sleep 0.5; adb shell input keyevent KEYCODE_ENTER # address, then the (empty) name field: Go connects
# a cable-attached device first tries USB for 10 s, and a loaded machine slows the rest: allow a minute
i=0; while [ $i -lt 120 ] && ! grep -q "streaming to" "$dlog"; do sleep 0.5; i=$((i + 1)); done
grep -q "streaming to" "$dlog" || fail "the device never connected"
first=$(sed -n 's/.*virtual output \([0-9]*x[0-9]*\)@.*/\1/p' "$dlog" | head -1)
w=${first%x*}; h=${first#*x}
[ "$w" -gt "$h" ] || fail "expected a landscape monitor first, got $first"

out() { niri msg --json outputs | jq -r 'to_entries[] | select(.value.serial=="PDISP0001") | .key'; }
name=$(out); niri msg output "$name" scale 1 >/dev/null 2>&1
"$pattern" -o "$name" -a -T -t 25 >"$art/pattern1.log" 2>&1 &
ppid=$!
sleep 6
adb exec-out screencap -p >"$art/landscape.png"
adb shell settings put system user_rotation 0 >/dev/null # portrait
i=0; while [ $i -lt 40 ] && ! grep -q "the phone confirmed epoch 1" "$dlog"; do sleep 0.5; i=$((i + 1)); done
grep -q "the phone confirmed epoch 1" "$dlog" || fail "the daemon never completed a reconfiguration"
second=$(sed -n 's/.*virtual output \([0-9]*x[0-9]*\)@.*/\1/p' "$dlog" | tail -1)
[ "$second" = "${h}x${w}" ] || fail "the monitor became $second, expected ${h}x${w}"
kill "$ppid" 2>/dev/null; wait "$ppid" 2>/dev/null; sleep 1.5
name=$(out); niri msg output "$name" scale 1 >/dev/null 2>&1
"$pattern" -o "$name" -a -T -t 20 >"$art/pattern2.log" 2>&1 &
ppid=$!
sleep 8
adb exec-out screencap -p >"$art/portrait.png"
status=$(adb logcat -d -s pdisp:* | grep -E "[0-9]+x[0-9]+ +[0-9]+ fps" | tail -1)
echo "$status" | cut -c30-
echo "$status" | grep -q "${h}x${w}" || fail "the app is not showing the portrait stream: $status"
fps=$(echo "$status" | sed -n 's/.* \([0-9]*\) fps.*/\1/p')
[ "${fps:-0}" -ge 50 ] || fail "only $fps fps after the rotation"
echo "PASS: rotated from ${w}x${h} to ${h}x${w} mid-stream, the monitor followed, $fps fps afterwards"
