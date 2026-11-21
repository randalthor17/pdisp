#!/usr/bin/env bash
# M9 check: where a window on the virtual monitor ends up when the phone goes and comes back (the plan's scenario matrix).
# It opens ONE window of our own test client (pdisp-pattern) on the virtual monitor, so it touches your desktop: when the monitor goes the
# compositor moves that window to a real monitor. Only runs with PDISP_TEST_WINDOWS=1. It restores the focus you had and closes the window.
# Every scenario is an observation of niri's behaviour; the lines starting "expect" are what the design says.
# usage: m9_windows_check.sh <pdispd> <pdisp-view> <pdisp-pattern> [artifact-dir]
set -u
daemon=$1; viewer=$2; pattern=$3
art=${4:-$(mktemp -d)}; mkdir -p "$art"; rm -rf "${art:?}"/*
skip() { echo "SKIP: $*"; exit 77; }
[ "${PDISP_TEST_WINDOWS:-}" = 1 ] || skip "opens a window on your desktop (and the compositor moves it); set PDISP_TEST_WINDOWS=1"
command -v niri >/dev/null 2>&1 && command -v jq >/dev/null 2>&1 || skip "niri or jq missing"
niri msg --json outputs >/dev/null 2>&1 || skip "no running niri session"
PORT=${PDISP_TEST_PORT:-47121}; SERIAL=PDISPWIN; GRACE=6
dpid=; vpid=; wpid=
prev_win=$(niri msg --json windows | jq -r '[.[] | select(.is_focused)][0].id // empty')
cleanup() {
	for p in $wpid $vpid $dpid; do [ -n "$p" ] && { kill -CONT "$p" 2>/dev/null; kill "$p" 2>/dev/null; }; done; wait 2>/dev/null
	[ -n "$prev_win" ] && niri msg action focus-window --id "$prev_win" >/dev/null 2>&1
}
trap cleanup EXIT
fail() { echo "FAIL: $*"; grep -v '^\[libevdi\|^libva' "$art/daemon.log" | tail -8; exit 1; }
present() { niri msg --json outputs 2>/dev/null | jq -e --arg s "$SERIAL" 'to_entries[] | select(.value.serial == $s)' >/dev/null 2>&1; }
connector() { niri msg --json outputs | jq -r --arg s "$SERIAL" 'to_entries[] | select(.value.serial == $s) | .key'; }
wait_present() { local i=0; while ! present; do sleep 0.1; i=$((i + 1)); [ $i -lt ${1:-100} ] || return 1; done; }
wait_gone() { local i=0; while present; do sleep 0.1; i=$((i + 1)); [ $i -lt $(( ${1:-15} * 10 )) ] || return 1; done; }
# the output the test window is on now ("none" when the window does not exist)
where() { niri msg --json windows | jq -r --argjson w "$(niri msg --json workspaces)" '[.[] | select(.app_id=="pdisp-pattern")][0] | if . == null then "none" else (.workspace_id as $i | ($w[] | select(.id==$i) | .output) // "?") end'; }
start_daemon() {
	"$daemon" -p "$PORT" -K "$art/pc" -P accept -t "$SERIAL" -n pdisp -g "$GRACE" >"$art/daemon.log" 2>&1 &
	dpid=$!
	local i=0; while [ $i -lt 40 ] && ! grep -q "listening on port" "$art/daemon.log"; do
		kill -0 "$dpid" 2>/dev/null || { wait "$dpid"; [ $? = 3 ] && skip "no free evdi device"; fail "daemon exited early"; }
		sleep 0.25; i=$((i + 1)); done
}
start_viewer() { "$viewer" 127.0.0.1 "$PORT" -i "$art/phone" -W 1080 -H 2400 -S 120 "$@" >"$art/viewer.log" 2>&1 & vpid=$!; }
lose_viewer() { kill -9 "$vpid" 2>/dev/null; wait "$vpid" 2>/dev/null; vpid=; }
open_window() { "$pattern" -o "$(connector)" -t 600 >"$art/window.log" 2>&1 & wpid=$!; local i=0; while [ "$(where)" = none ] && [ $i -lt 50 ]; do sleep 0.2; i=$((i + 1)); done; }
close_window() { [ -n "$wpid" ] && { kill "$wpid" 2>/dev/null; wait "$wpid" 2>/dev/null; wpid=; }; }
real=$(niri msg --json workspaces | jq -r '[.[] | select(.is_focused)][0].output') # where the windows should land: the monitor you are working on
echo "real monitor: $real"

start_daemon
start_viewer
wait_present 150 || fail "no virtual output"
virt=$(connector); sleep 2
open_window
w=$(where); echo "the window opened on: $w (virtual output is $virt)"
[ "$w" = "$virt" ] || fail "could not put the test window on the virtual monitor (it is on $w)"
[ -n "$prev_win" ] && niri msg action focus-window --id "$prev_win" >/dev/null 2>&1

echo "-- A. the phone is lost and comes back within the grace period"
lose_viewer; sleep 2
echo "   during the grace period the window is on: $(where)   expect: $virt"
start_viewer; sleep 3
echo "   after the phone returned it is on: $(where)   expect: $virt"
[ "$(where)" = "$virt" ] || fail "the window moved although the monitor never left"

echo "-- B. the phone is lost and does not come back (the app was closed)"
lose_viewer; wait_gone 15 || fail "the monitor stayed"
sleep 1
echo "   after the grace period the monitor is gone, the window is on: $(where)   expect: $real"
[ "$(where)" = "$real" ] || fail "the window did not move to the real monitor"

echo "-- C. the phone comes back after the monitor was removed (resume with the app open)"
start_viewer; wait_present 150 || fail "the monitor did not return"
sleep 2
echo "   the monitor is back as $(connector); the window is on: $(where)   expect: $(connector) (niri gives a workspace back to its output)"
c_where=$(where)

echo "-- D. the phone locks (CLOSE reason 1): at once"
stop_c=$(connector)
lose_viewer; sleep 0.1
start_viewer -C 1 -S 3 2>/dev/null; wait_present 100 >/dev/null 2>&1; sleep 4   # the viewer connects, then sends CLOSE reason 1 when its -S time ends
wait_gone 5 >/dev/null 2>&1
echo "   after the lock the window is on: $(where)   expect: $real"

echo "-- E. the daemon is restarted while the monitor holds the window"
start_viewer -S 120; wait_present 150 || fail "the monitor did not return after the lock"
sleep 2; echo "   (window before: $(where))"
kill "$dpid"; wait "$dpid" 2>/dev/null; dpid=
wait_gone 5 || fail "the monitor stayed after the daemon stopped"
sleep 1; echo "   the monitor is gone, the window is on: $(where)   expect: $real"
start_daemon; start_viewer -S 30
wait_present 150 || fail "the monitor did not return after the daemon restarted"
sleep 2; echo "   the daemon is back, the monitor is $(connector); the window is on: $(where)"
close_window
echo "DONE (window closed, focus restored)"
