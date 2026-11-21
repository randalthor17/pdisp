#!/usr/bin/env bash
# M9 check: the life of the virtual monitor around a phone that goes away. Only the monitor's existence is watched (niri msg outputs), so no
# window is opened and nothing on your desktop is touched. The daemon runs with a 6 s grace period.
#   - phone lost (process killed, as when a cable is pulled): the monitor stays for the grace period, then goes
#   - phone silent (frozen, as when Wi-Fi drops without a goodbye): noticed in a few seconds, then the same grace period
#   - the phone comes back within the grace period: the same monitor, never gone in between, and it streams again
#   - it comes back too late: a new monitor
#   - the phone locks its screen (CLOSE reason 1) or disconnects on purpose: the monitor goes at once
#   - the phone comes back needing another size: the monitor follows and streams
#   - the daemon is restarted: the monitor goes, and returns with the same identity
# Needs niri, jq, a free evdi device and a VAAPI HEVC encoder. Exits 77 (meson: skipped) otherwise.
# usage: m9_lifecycle_check.sh <pdispd> <pdisp-view> [artifact-dir]
set -u
daemon=$1; viewer=$2
art=${3:-$(mktemp -d)}; mkdir -p "$art"; rm -rf "${art:?}"/*
skip() { echo "SKIP: $*"; exit 77; }
command -v niri >/dev/null 2>&1 && command -v jq >/dev/null 2>&1 || skip "niri or jq missing"
niri msg --json outputs >/dev/null 2>&1 || skip "no running niri session"
PORT=${PDISP_TEST_PORT:-47119}; SERIAL=PDISPLIFE; GRACE=6
dpid=; vpid=
cleanup() { for p in $vpid $dpid; do [ -n "$p" ] && { kill -CONT "$p" 2>/dev/null; kill "$p" 2>/dev/null; }; done; wait 2>/dev/null; }
trap cleanup EXIT
fail() { echo "FAIL: $*"; echo "--- daemon:"; grep -v '^\[libevdi\|^libva' "$art/daemon.log" | tail -12; echo "--- viewer:"; tail -5 "$art/viewer.log"; exit 1; }
now() { date +%s%N; }
ms_since() { echo $(( ($(now) - $1) / 1000000 )); }
present() { niri msg --json outputs 2>/dev/null | jq -e --arg s "$SERIAL" 'to_entries[] | select(.value.serial == $s)' >/dev/null 2>&1; }
connector() { niri msg --json outputs | jq -r --arg s "$SERIAL" 'to_entries[] | select(.value.serial == $s) | .key'; }
wait_present() { local i=0; while ! present; do sleep 0.1; i=$((i + 1)); [ $i -lt ${1:-100} ] || return 1; done; }
# waits until the monitor is gone; prints how long it took in ms (or fails after $1 seconds)
wait_gone() { local t0; t0=$(now); while present; do sleep 0.1; [ "$(ms_since "$t0")" -lt $(( ${1:-15} * 1000 )) ] || return 1; done; ms_since "$t0"; }

start_daemon() {
	"$daemon" -p "$PORT" -K "$art/pc" -P accept -t "$SERIAL" -n pdisp -g "$GRACE" >"$art/daemon.log" 2>&1 &
	dpid=$!
	local i=0; while [ $i -lt 40 ] && ! grep -q "listening on port" "$art/daemon.log"; do
		kill -0 "$dpid" 2>/dev/null || { wait "$dpid"; [ $? = 3 ] && skip "no free evdi device"; fail "daemon exited early"; }
		sleep 0.25; i=$((i + 1)); done
}
start_viewer() { # start_viewer <W> <H> [extra args]
	local w=$1 h=$2; shift 2
	"$viewer" 127.0.0.1 "$PORT" -i "$art/phone" -W "$w" -H "$h" -S 60 "$@" >"$art/viewer.log" 2>&1 &
	vpid=$!
}
decoded() { sed -n 's/^VIEW .* decoded=\([0-9]*\) .*/\1/p' "$art/viewer.log"; }
stop_viewer() { [ -n "$vpid" ] && { kill "$vpid" 2>/dev/null; wait "$vpid" 2>/dev/null; vpid=; }; }
lose_viewer() { kill -9 "$vpid" 2>/dev/null; wait "$vpid" 2>/dev/null; vpid=; }  # vanishes without a goodbye

start_daemon
echo "-- a phone connects"
start_viewer 1080 2400
wait_present 150 || fail "the virtual output never appeared"
first=$(connector); sleep 2

echo "-- 1. the phone is lost: the monitor waits for the grace period, then goes"
t0=$(now); lose_viewer
sleep 3
present || fail "the monitor vanished $(ms_since "$t0") ms after the phone was lost, during the $GRACE s grace period"
gone=$(wait_gone 15) || fail "the monitor was still there 15 s after the phone was lost"
total=$(ms_since "$t0")
[ "$total" -ge $(( GRACE * 1000 - 800 )) ] && [ "$total" -le $(( GRACE * 1000 + 3500 )) ] || fail "the monitor went after $total ms, expected about $GRACE s"
grep -q "keeping the virtual output" "$art/daemon.log" || fail "the daemon did not say it was waiting"
echo "   ok: it stayed $total ms (grace $GRACE s), then went"

echo "-- 2. back within the grace period: the same monitor, never gone, and streaming"
start_viewer 1080 2400
wait_present 150 || fail "the monitor did not return"
sleep 2
t0=$(now); lose_viewer; sleep 2
start_viewer 1080 2400 -S 6
gaps=0; for i in $(seq 1 40); do present || gaps=$((gaps + 1)); sleep 0.1; done
[ "$gaps" = 0 ] || fail "the monitor was gone $gaps times while the phone reconnected within the grace period"
grep -q "still there: reusing it" "$art/daemon.log" || fail "the daemon did not reuse the monitor"
[ "$(connector)" = "$first" ] || fail "the monitor changed connector ($first -> $(connector))"
sleep 4; wait "$vpid" 2>/dev/null; vpid=
d=$(decoded); [ "${d:-0}" -ge 1 ] || fail "nothing decoded after the reconnect"
echo "   ok: same monitor ($first), reused, $d frames decoded after coming back"
wait_gone 12 >/dev/null || fail "the monitor stayed after the phone left on purpose"   # (the second viewer ended with a normal close)

echo "-- 3. back too late: a new monitor"
start_viewer 1080 2400
wait_present 150 || fail "the monitor did not appear"
sleep 2; lose_viewer
wait_gone 15 >/dev/null || fail "the monitor never went"
start_viewer 1080 2400 -S 5
wait_present 150 || fail "no new monitor after the grace period had passed"
grep -c "the virtual output 1080x2400 is still there" "$art/daemon.log" | grep -q "^1$" || true
sleep 6; stop_viewer
wait_gone 12 >/dev/null
echo "   ok"

echo "-- 4. the phone's screen locks (CLOSE reason 1): the monitor goes at once, no grace period"
start_viewer 1080 2400 -S 5 -C 1
wait_present 150 || fail "the monitor did not appear"
t0=$(now)
wait "$vpid" 2>/dev/null; vpid=
gone=$(wait_gone 8) || fail "the monitor stayed after the screen locked"
total=$(ms_since "$t0")
[ "$total" -le 9500 ] || fail "after a lock the monitor took $total ms to go (viewer ends 5 s after streaming starts)"
grep -q "its screen locked" "$art/daemon.log" || fail "the daemon did not recognise the lock"
grep -q "removing the virtual output" "$art/daemon.log" || fail "the daemon did not remove the output"
echo "   ok: gone ${gone} ms after the close"

echo "-- 5. the phone disconnects on purpose: gone at once"
start_viewer 1080 2400 -S 4
wait_present 150 || fail "the monitor did not appear"
wait "$vpid" 2>/dev/null; vpid=
gone=$(wait_gone 8) || fail "the monitor stayed after a deliberate disconnect"
grep -q "(on purpose)" "$art/daemon.log" || fail "the daemon did not see a deliberate close"
echo "   ok: gone ${gone} ms after the close"

echo "-- 6. a silent phone (frozen, no goodbye): noticed, then the grace period"
start_viewer 1080 2400
wait_present 150 || fail "the monitor did not appear"
sleep 2; t0=$(now); kill -STOP "$vpid"
i=0; while ! grep -q "no packets from the phone" "$art/daemon.log" && [ $i -lt 100 ]; do sleep 0.1; i=$((i + 1)); done
noticed=$(ms_since "$t0")
[ "$noticed" -ge 2000 ] && [ "$noticed" -le 6000 ] || fail "a frozen phone was noticed after $noticed ms (expected 3 to 4 s)"
present || fail "the monitor was gone as soon as the phone went silent"
gone=$(wait_gone 15) || fail "the monitor never went after the phone froze"
kill -CONT "$vpid" 2>/dev/null; lose_viewer
echo "   ok: noticed after $noticed ms, monitor gone after $(ms_since "$t0") ms in all"

echo "-- 7. back within the grace period but needing another size (rotated)"
start_viewer 1080 2400
wait_present 150 || fail "the monitor did not appear"
sleep 2; lose_viewer
start_viewer 2400 1080 -S 6
sleep 5
grep -q "virtual output 2400x1080" "$art/daemon.log" || fail "the monitor did not follow the new size"
sleep 3; wait "$vpid" 2>/dev/null; vpid=
d=$(decoded); [ "${d:-0}" -ge 1 ] || fail "nothing decoded at the new size"
wait_gone 12 >/dev/null
echo "   ok"

echo "-- 8. the daemon restarts: the monitor goes, and returns with the same identity"
start_viewer 1080 2400
wait_present 150 || fail "the monitor did not appear"
sleep 2; lose_viewer
kill "$dpid"; wait "$dpid" 2>/dev/null; dpid=
gone=$(wait_gone 8) || fail "the monitor stayed after the daemon was stopped"
start_daemon
start_viewer 1080 2400 -S 4
wait_present 150 || fail "the monitor did not come back with the restarted daemon"
present || fail "identity lost"
sleep 5; stop_viewer
echo "   ok: gone ${gone} ms after the daemon stopped, back under the same serial"
echo "PASS: the virtual monitor follows the phone's presence as designed: grace period for a lost phone, at once for a goodbye or a lock, reuse on a quick return"
