#!/usr/bin/env bash
# M13 check: the phone's brightness is set over DDC/CI. The virtual monitor answers on the I2C bus of its evdi device, so `ddcutil` (and a desktop
# shell's brightness slider) can read and set it. Expected:
#   1. with a phone connected, ddcutil reads VCP 0x10 (brightness) as 100 of 100
#   2. setting it to 35 reaches the phone as a BRIGHTNESS message, and reading it back gives 35
#   3. a phone that connects later is told the level at once (the host set it earlier)
# Needs ddcutil, an evdi device, and read/write access to its /dev/i2c-N (the i2c group). Exits 77 (skipped) otherwise. It makes no output in your
# compositor beyond the virtual one, and needs no niri.
# usage: m13_brightness_check.sh <pdispd> <pdisp-view> [artifact-dir]
set -u
daemon=$1; viewer=$2
art=${3:-$(mktemp -d)}; mkdir -p "$art"; rm -rf "${art:?}"/*
skip() { echo "SKIP: $*"; exit 77; }
command -v ddcutil >/dev/null 2>&1 || skip "ddcutil missing"
PORT=${PDISP_TEST_PORT:-24123}; SERIAL=PDISPM13CHK
dpid=; vpid=
cleanup() { for p in $vpid $dpid; do [ -n "$p" ] && kill "$p" 2>/dev/null; done; wait 2>/dev/null; }
trap cleanup EXIT
fail() { echo "FAIL: $*"; echo "--- daemon:"; grep -v '^\[libevdi\]\|^libva' "$art/daemon.log" | tail -15; echo "--- viewer:"; tail -6 "$art/viewer.log" 2>/dev/null; exit 1; }

PDISP_DDC_DEBUG=1 "$daemon" -K "$art/pc" -P accept -p "$PORT" -t "$SERIAL" -n pdisp -g 0 >"$art/daemon.log" 2>&1 &
dpid=$!
sleep 0.7
if ! kill -0 "$dpid" 2>/dev/null; then wait "$dpid"; [ $? = 3 ] && skip "no free evdi device"; fail "daemon exited early"; fi

phone() { "$viewer" 127.0.0.1 "$PORT" -i "$art/phone" -W 1280 -H 800 -S "$1" >"$art/viewer.log" 2>&1 & vpid=$!; }
bus_of() { # the I2C bus of the evdi card the daemon uses
	local card; card=$(sed -n 's/.*using evdi card\([0-9]*\).*/\1/p' "$art/daemon.log" | head -1)
	[ -n "$card" ] || return 1
	local d; d=$(ls -d /sys/class/drm/card"$card"/device/i2c-* 2>/dev/null | head -1)
	[ -n "$d" ] && echo "${d##*i2c-}"
}
vcp() { ddcutil --bus "$1" getvcp 10 2>&1; }

echo "-- 1. a connected phone: the brightness reads as 100"
phone 30
i=0; bus=
while [ $i -lt 50 ]; do grep -q "virtual output" "$art/daemon.log" && bus=$(bus_of) && [ -n "$bus" ] && break; sleep 0.2; i=$((i + 1)); done
[ -n "$bus" ] || fail "no virtual output / no I2C bus for it"
[ -r "/dev/i2c-$bus" ] && [ -w "/dev/i2c-$bus" ] || skip "no access to /dev/i2c-$bus (the i2c group)"
sleep 1
out=$(vcp "$bus")
if ! echo "$out" | grep -q "current value =   *100, max value =   *100"; then
	echo "--- ddcutil said: $out"; echo "--- verbose:"; ddcutil --bus "$bus" getvcp 10 --verbose 2>&1 | head -40
	echo "--- with the checks skipped:"; ddcutil --bus "$bus" getvcp 10 --skip-ddc-checks 2>&1 | head -10
	echo "--- daemon DDC traffic:"; grep "^\[ddc\]" "$art/daemon.log" | head -20
	fail "unexpected brightness reading: $out"
fi
echo "   ok: $out"

echo "-- 2. setting it reaches the phone, and reads back"
ddcutil --bus "$bus" setvcp 10 35 >"$art/set.log" 2>&1 || fail "ddcutil setvcp failed: $(cat "$art/set.log")"
sleep 1
grep -q "BRIGHTNESS: 35 %" "$art/viewer.log" || fail "the phone was not told"
out=$(vcp "$bus"); echo "$out" | grep -q "current value =   *35," || fail "reads back as: $out"
echo "   ok"
kill "$vpid" 2>/dev/null; wait "$vpid" 2>/dev/null; vpid=
sleep 2

echo "-- 3. a phone that connects later is told at once"
phone 6
sleep 4
grep -q "BRIGHTNESS: 35 %" "$art/viewer.log" || fail "the new phone was not told the level"
echo "   ok"
echo "PASS: brightness is read and set over DDC/CI, and the phone follows"
