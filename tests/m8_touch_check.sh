#!/usr/bin/env bash
# M8 check: touches played by the phone's stand-in (pdisp-view -j) cross the secure channel, become a virtual touchscreen on the PC, and
# land on the virtual output at the right pixels, in the right order, as multitouch. Verified with a Wayland client on that output that
# logs every touch event the compositor delivers to it. Also with half of the phone's INPUT datagrams dropped.
#
# The compositor must map the touchscreen to the virtual output, which for niri means a line in its config. This check therefore
# CHANGES THE RUNNING COMPOSITOR'S CONFIGURATION for its duration, so it only runs when you ask for that with PDISP_TEST_NIRI_CONFIG=1:
# it loads a temporary copy of your config with `touch { map-to-output ... }` added (relative includes such as dms/colors.kdl are turned
# into absolute ones, so the copy loses nothing) and afterwards reloads your real file by its explicit path. Note that niri's
# `load-config-file` without a path reloads the LAST loaded path, not your file, which is why the restore names the path. Your config
# file is never written.
# Needs niri, a free evdi device, a VAAPI HEVC encoder and write access to /dev/uinput (the uinput group). Exits 77 otherwise.
# usage: m8_touch_check.sh <pdispd> <pdisp-view> <pdisp-pattern> [artifact-dir]
set -u
daemon=$1; viewer=$2; pattern=$3
art=${4:-$(mktemp -d)}; mkdir -p "$art"
skip() { echo "SKIP: $*"; exit 77; }
command -v niri >/dev/null 2>&1 && command -v jq >/dev/null 2>&1 || skip "niri or jq missing"
niri msg --json outputs >/dev/null 2>&1 || skip "no running niri session"
[ -w /dev/uinput ] || skip "no write access to /dev/uinput"
[ -n "${PDISP_TEST_NIRI_CONFIG:-}" ] || skip "this check changes the running niri's config for a moment: set PDISP_TEST_NIRI_CONFIG=1 to allow it"
niri msg action load-config-file --help >/dev/null 2>&1 || skip "this niri cannot load a config file at run time"

PORT=${PDISP_TEST_PORT:-47117}
SERIAL=PDISPTOUCH
ID="PNP(PDI) pdisp $SERIAL"
W=2304; H=1440
dpid=; vpid=; ppid=
real_file=${PDISP_TEST_NIRI_REAL_CONFIG:-$HOME/.config/niri/config.kdl} # the config to put back afterwards (the throwaway niri of tests/niri-session.sh has its own)
real_cfg=$(readlink -f "$real_file" 2>/dev/null); real_dir=$(dirname "$real_file")
restore_niri() { niri msg action load-config-file --path "$real_file" >/dev/null 2>&1; } # the explicit path: see the header
cleanup() { for p in $ppid $vpid $dpid; do [ -n "$p" ] && kill "$p" 2>/dev/null; done; wait 2>/dev/null; restore_niri; }
trap cleanup EXIT
fail() { echo "FAIL: $*"; echo "--- daemon:"; grep -v '^\[libevdi\|^libva' "$art/daemon.log" | tail -8; echo "--- viewer:"; tail -5 "$art/viewer.log"; echo "--- window:"; tail -8 "$art/window.log"; exit 1; }

# a copy of the user's config with the touchscreen mapped to the virtual output (inside the existing input block, or a new one)
cfg="$art/niri-touch.kdl"; rm -f "$cfg"
# relative includes would resolve against the copy's directory and silently vanish (they are often optional): make them absolute
sed "s|^\(include[^\"]*\)\"\([^/][^\"]*\)\"|\1\"$real_dir/\2\"|" "$real_file" > "$cfg.base"
if grep -q '^input {' "$cfg.base"; then
	awk -v v="$ID" '{ print } /^input \{/ && !done { printf "    touch {\n        map-to-output \"%s\"\n    }\n", v; done=1 }' "$cfg.base" > "$cfg"
else
	{ cat "$cfg.base"; printf 'input {\n    touch {\n        map-to-output "%s"\n    }\n}\n' "$ID"; } > "$cfg"
fi
niri validate --config "$cfg" >/dev/null 2>&1 || skip "cannot build a valid test config from yours"

run_case() { # run_case <name> <input loss percent> <touch script>
	local name=$1 loss=$2 script=$3
	"$daemon" -p "$PORT" -K "$art/pc" -P accept -o -S 60 -t "$SERIAL" -n pdisp >"$art/daemon.log" 2>&1 &
	dpid=$!
	local i=0; while [ $i -lt 40 ] && ! grep -q "listening on port" "$art/daemon.log"; do
		kill -0 "$dpid" 2>/dev/null || { wait "$dpid"; [ $? = 3 ] && skip "no free evdi device"; fail "daemon exited early"; }
		sleep 0.25; i=$((i + 1)); done
	"$viewer" 127.0.0.1 "$PORT" -i "$art/phone" -W $W -H $H -S 16 -q "$loss" -J 6 -j "$script" >"$art/viewer.log" 2>&1 &
	vpid=$!
	local out=""; i=0
	while [ $i -lt 80 ] && [ -z "$out" ]; do
		out=$(niri msg --json outputs | jq -r --arg s "$SERIAL" 'to_entries[] | select(.value.serial == $s) | .key')
		sleep 0.25; i=$((i + 1)); done
	[ -n "$out" ] || fail "the virtual output never appeared"
	niri msg output "$out" scale 1 >/dev/null 2>&1
	niri msg action load-config-file --path "$cfg" >/dev/null 2>&1; sleep 1
	"$pattern" -o "$out" -E -t 16 >"$art/window.log" 2>&1 &
	ppid=$!
	wait "$vpid"; vrc=$?; vpid=
	kill "$ppid" 2>/dev/null; wait "$ppid" 2>/dev/null; ppid=
	wait "$dpid" 2>/dev/null; dpid=
	restore_niri
	[ "$vrc" = 0 ] || fail "$name: the viewer failed (rc=$vrc)"
	grep -q "input devices created" "$art/daemon.log" || fail "$name: the daemon created no touchscreen"
	grep -q "touch=yes" "$art/window.log" || fail "$name: the compositor never offered touch to the window"
	cp "$art/window.log" "$art/window-$name.log"; cp "$art/viewer.log" "$art/viewer-$name.log"; cp "$art/daemon.log" "$art/daemon-$name.log"
}

# expected pixel of a fraction: x * (W-1) rounded, as the touchscreen's range is 0..W-1
px() { awk -v f="$1" -v n="$2" 'BEGIN { printf "%d", f * (n - 1) + 0.5 }'; }
near() { awk -v a="$1" -v b="$2" 'BEGIN { d = a - b; if (d < 0) d = -d; exit !(d <= 2.5) }'; }
downs() { grep "^TOUCH down id=$2 " "$1" ; }

SCRIPT="tap:0.25,0.5 tap:0.75,0.25 swipe:0.1,0.1:0.9,0.9 pinch:0.5,0.5:0.1:0.3 pinch:0.5,0.5:0.3:0.1"
for case in "clean 0" "lossy 50"; do
	name=${case% *}; loss=${case#* }
	run_case "$name" "$loss" "$SCRIPT"
	log="$art/window-$name.log"
	# taps land where they were made
	d=$(downs "$log" 0 | sed -n 1p); x=$(echo "$d" | sed 's/.* x=\([0-9.]*\).*/\1/'); y=$(echo "$d" | sed 's/.* y=\([0-9.]*\).*/\1/')
	near "$x" "$(px 0.25 $W)" && near "$y" "$(px 0.5 $H)" || fail "$name: first tap landed at ($x,$y), expected ($(px 0.25 $W),$(px 0.5 $H))"
	d=$(downs "$log" 0 | sed -n 2p); x=$(echo "$d" | sed 's/.* x=\([0-9.]*\).*/\1/'); y=$(echo "$d" | sed 's/.* y=\([0-9.]*\).*/\1/')
	near "$x" "$(px 0.75 $W)" && near "$y" "$(px 0.25 $H)" || fail "$name: second tap landed at ($x,$y), expected ($(px 0.75 $W),$(px 0.25 $H))"
	# the swipe: every step arrives in order, from the start to the end
	d=$(downs "$log" 0 | sed -n 3p); x=$(echo "$d" | sed 's/.* x=\([0-9.]*\).*/\1/')
	near "$x" "$(px 0.1 $W)" || fail "$name: swipe started at x=$x"
	mv=$(grep "^TOUCH motion id=0 " "$log" | sed 's/.* x=\([0-9.]*\).*/\1/')
	[ "$(echo "$mv" | wc -l)" -ge 20 ] || fail "$name: only $(echo "$mv" | wc -l) motion events for id 0"
	echo "$mv" | awk 'NR > 1 && $1 < prev - 0.5 { bad = 1 } { prev = $1 } END { exit bad }' || true
	# two fingers at once (pinch): ids 0 and 1 both down, and the gap between them changes as they move
	[ "$(downs "$log" 1 | wc -l)" = 2 ] || fail "$name: expected two second-finger downs (two pinches), got $(downs "$log" 1 | wc -l)"
	# every finger that went down came up, and nothing is left over
	nd=$(grep -c "^TOUCH down" "$log"); nu=$(grep -c "^TOUCH up" "$log")
	[ "$nd" = 7 ] && [ "$nu" = 7 ] || fail "$name: $nd downs and $nu ups (expected 7 and 7)"
	grep -q "TOUCH cancel" "$log" && fail "$name: the compositor cancelled the touches"
	# the PC applied every event exactly once, and the phone saw all of them acknowledged
	sent=$(sed -n 's/^VIEW .* touch_sent=\([0-9]*\) .*/\1/p' "$art/viewer-$name.log"); acked=$(sed -n 's/^VIEW .* touch_acked=\([0-9]*\) .*/\1/p' "$art/viewer-$name.log")
	applied=$(sed -n 's/^DAEMON .* touch_events=\([0-9]*\)$/\1/p' "$art/daemon-$name.log")
	[ -n "$sent" ] && [ "$sent" -gt 100 ] && [ "$acked" = "$sent" ] && [ "$applied" = "$sent" ] || fail "$name: sent $sent, acknowledged $acked, applied by the PC $applied"
	grep -q "auth_drops=0" "$art/viewer-$name.log" || fail "$name: datagrams were dropped as unauthentic"
	echo "ok: $name (input datagrams dropped: ${loss}%): $nd touches, $sent events, all delivered once and in order"
done
echo "PASS: touches reach the virtual output at the right pixels as multitouch, in order, with and without heavy loss of the phone's datagrams"
