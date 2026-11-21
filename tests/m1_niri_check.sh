#!/bin/sh
# M1 pass check against a running niri session:
#   1. the evdi output appears with the requested mode, make/model/serial and physical size
#   2. after disconnect and reconnect it comes back under the same name and identity
# Needs niri, jq and a free evdi device. Exits 77 (meson: skipped) when those are missing.
# usage: m1_niri_check.sh <pdisp-vdisplay>
set -u
tool=$1

skip() { echo "SKIP: $*"; exit 77; }
command -v niri >/dev/null 2>&1 || skip "niri not installed"
command -v jq >/dev/null 2>&1 || skip "jq not installed"
niri msg --json outputs >/dev/null 2>&1 || skip "no running niri session"

SERIAL=PDISPM1CHK
log=$(mktemp)
"$tool" -m 2304x1440@90 -s 234x150 -n pdisp-tablet -t "$SERIAL" -N 7 -c 2 -u 8 -g 4 >"$log" 2>&1 &
pid=$!
trap 'kill $pid 2>/dev/null; wait $pid 2>/dev/null; rm -f "$log"' EXIT

# print the niri output entry for our serial as one JSON object, or nothing
find_output() {
	niri msg --json outputs 2>/dev/null |
		jq -c --arg s "$SERIAL" 'to_entries | map(select(.value.serial == $s)) | first // empty
			| {name: .key, make: .value.make, model: .value.model, serial: .value.serial,
			   phys: .value.physical_size, w: .value.modes[.value.current_mode].width,
			   h: .value.modes[.value.current_mode].height,
			   mhz: .value.modes[.value.current_mode].refresh_rate}'
}

wait_for() { # wait_for present|absent seconds
	i=0
	while [ "$i" -lt $(($2 * 5)) ]; do
		out=$(find_output)
		if [ "$1" = present ] && [ -n "$out" ]; then echo "$out"; return 0; fi
		if [ "$1" = absent ] && [ -z "$out" ]; then return 0; fi
		sleep 0.2
		i=$((i + 1))
	done
	return 1
}

fail() { echo "FAIL: $*"; echo "--- tool log:"; cat "$log"; exit 1; }

sleep 0.5
if ! kill -0 $pid 2>/dev/null; then
	wait $pid; rc=$?
	[ "$rc" = 3 ] && skip "no free evdi device"
	fail "tool exited early (rc=$rc)"
fi

first=$(wait_for present 6) || fail "output with serial $SERIAL never appeared in niri"
echo "first:  $first"

echo "$first" | jq -e '.w == 2304 and .h == 1440' >/dev/null || fail "wrong mode: $first"
echo "$first" | jq -e '(.mhz - 90000 | fabs) < 200' >/dev/null || fail "wrong refresh: $first"
echo "$first" | jq -e '.phys == [230, 150]' >/dev/null || fail "wrong physical size (want 230x150 mm from 23x15 cm): $first"
echo "$first" | jq -e '.model == "pdisp-tablet"' >/dev/null || fail "wrong model: $first"
echo "$first" | jq -e '.make == "PNP(PDI)"' >/dev/null || fail "wrong make: $first"

wait_for absent 15 || fail "output did not disappear after disconnect"
echo "disconnected: output gone"

second=$(wait_for present 12) || fail "output did not come back after reconnect"
echo "second: $second"
[ "$first" = "$second" ] || fail "identity changed across reconnect"

echo "PASS: output appeared with the right mode, make/model/serial and size; reconnect kept the same identity"
