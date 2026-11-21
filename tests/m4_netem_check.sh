#!/bin/sh
# M4 acceptance: the real daemon and viewer over an emulated bad link. Each scenario runs the loopback check inside an
# unprivileged user and network namespace (`unshare -Urn`) with netem on the loopback device, applied to UDP only so the
# control connection stays on a clean path. Acceptance (the plan): under bursty loss, jitter and a bandwidth cap there is
# no persistent corruption, the picture recovers within bounded time, and latency does not grow.
# Needs unshare, tc, ip, niri and the loopback check's requirements. Exits 77 (meson: skipped) when they are missing.
# usage: m4_netem_check.sh <pdispd> <pdisp-view> <pdisp-pattern> [artifact-dir]
set -u
daemon=$1; viewer=$2; pattern=$3
art=${4:-$(mktemp -d)}
here=$(cd "$(dirname "$0")" && pwd)

skip() { echo "SKIP: $*"; exit 77; }
for t in unshare tc ip; do command -v "$t" >/dev/null 2>&1 || skip "$t not installed"; done
unshare -Urn true 2>/dev/null || skip "cannot create an unprivileged user and network namespace"

failed=0
# run_scenario <name> <netem arguments> -- <NAME=value ...threshold overrides>
run_scenario() {
	name=$1; netem=$2; shift 3
	dir="$art/$(echo "$name" | tr ' /' '__')"
	mkdir -p "$dir"
	echo "== $name: netem $netem"
	env "$@" NETEM="$netem" unshare -Urn sh -c '
		ip link set lo up &&
		tc qdisc add dev lo root handle 1: prio bands 3 priomap 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 &&
		tc qdisc add dev lo parent 1:3 handle 30: netem $NETEM &&
		tc filter add dev lo parent 1: protocol ip prio 1 u32 match ip protocol 17 0xff flowid 1:3 || exit 99
		exec "$0" "$1" "$2" "$3" "$4"' "$here/m4_loopback_check.sh" "$daemon" "$viewer" "$pattern" "$dir"
	rc=$?
	case $rc in
		0) ;;
		77) echo "SKIP: $name (no live session, or the desktop was disturbed during the run)"; exit 77 ;;
		*) echo "FAILED: $name (rc=$rc)"; failed=1 ;;
	esac
}

# The limits are acceptance bounds, not benchmarks: real-time netem runs vary by tens of per cent from run to run on this machine
# (the same commit measured p99 latency 38 to 75 ms in the 8 ms scenario and freezes of 37 to 446 ms in the 15 Mbit/s one), so they
# are set to catch a broken recovery or rate controller, not a slightly slow run.
run_scenario "latency only, 8 ms each way" "delay 8ms" -- \
	MIN_DECODED=540 MIN_FPS=55 MAX_P99_MS=100 MAX_FREEZE_MS=200 MAX_GIVEN_UP=0 GLASS_MAX_MS=120 OFFSET_TOL_US=4000
run_scenario "bursty ~2% loss, 15 ms jitter, 40 Mbit/s cap" \
	"delay 10ms 15ms loss gemodel 0.7% 25% 80% 0.1% rate 40mbit limit 330" -- \
	MIN_DECODED=540 MIN_FPS=50 MAX_P99_MS=150 MAX_FREEZE_MS=600 MAX_IDR=3 GLASS_MAX_MS=200 OFFSET_TOL_US=20000
run_scenario "bursty ~5% loss, 15 ms jitter" \
	"delay 10ms 15ms loss gemodel 2% 25% 80% 0.1%" -- \
	MIN_DECODED=480 MIN_FPS=45 MAX_P99_MS=200 MAX_FREEZE_MS=400 MAX_IDR=4 MIN_CHECK_PCT=90 GLASS_MAX_MS=250 OFFSET_TOL_US=20000
run_scenario "15 Mbit/s cap (the target starts at 20)" "delay 5ms rate 15mbit limit 150" -- \
	MIN_DECODED=430 MIN_FPS=50 MAX_P99_MS=150 MAX_FREEZE_MS=400 MAX_IDR=3 GLASS_MAX_MS=200 OFFSET_TOL_US=20000

[ "$failed" = 0 ] || exit 1
echo "PASS: no persistent corruption, bounded freezes and bounded latency under every impairment"
