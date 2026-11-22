#!/usr/bin/env bash
# M12 check: the daemon finds a refresh rate the device can sustain. The user's cap (90 Hz) is a ceiling; the stand-in phone offers 60 and 90 Hz and
# loses 20% of the datagrams while the stream is above 60 Hz (a device that cannot keep up with the higher rate). Expected:
#   1. the daemon starts at 90 Hz, sees the stream is not healthy and renegotiates to 60 Hz (RECONFIGURE), which the viewer follows
#   2. after a stretch of healthy streaming at 60 Hz it remembers 60 Hz for this device (<config dir>/learned_rates)
#   3. the next connection starts at 60 Hz at once
#   4. setting a new cap through the control socket forgets what was learned
# Needs niri, jq and an evdi device; it creates virtual outputs (run it in tests/niri-session.sh). Exits 77 (skipped) otherwise.
# usage: m12_rate_step_check.sh <pdispd> <pdisp-view> <pdisp-pattern> <pdisp-ctl> [artifact-dir]
set -u
daemon=$1; viewer=$2; pattern=$3; ctl=$4
art=${5:-$(mktemp -d)}; mkdir -p "$art"; rm -rf "${art:?}"/*
skip() { echo "SKIP: $*"; exit 77; }
[ -z "${PDISP_TEST_SLOW:-}" ] || skip "this check is about timing (a rate the device can sustain), which the sanitizers change"
command -v niri >/dev/null 2>&1 && command -v jq >/dev/null 2>&1 || skip "niri or jq missing"
niri msg --json outputs >/dev/null 2>&1 || skip "no running niri session"
PORT=${PDISP_TEST_PORT:-24122}; SERIAL=PDISPM12CHK; SOCK="$art/ctl.sock"
dpid=; vpid=; ppid=
cleanup() { for p in $ppid $vpid $dpid; do [ -n "$p" ] && kill "$p" 2>/dev/null; done; wait 2>/dev/null; }
trap cleanup EXIT
fail() { echo "FAIL: $*"; echo "--- daemon:"; grep -v '^\[libevdi\]\|^libva' "$art/daemon.log" | tail -15; echo "--- viewer:"; tail -6 "$art/viewer.log"; exit 1; }

"$daemon" -K "$art/pc" -P accept -p "$PORT" -t "$SERIAL" -n pdisp -g 0 -R 90 -C "$SOCK" -M >"$art/daemon.log" 2>&1 &
dpid=$!
sleep 0.7
if ! kill -0 "$dpid" 2>/dev/null; then wait "$dpid"; [ $? = 3 ] && skip "no free evdi device"; fail "daemon exited early"; fi

run_viewer() { # run_viewer <seconds> [extra viewer args]: a phone that offers 60 and 90 Hz and cannot cope above 60
	local secs=$1; shift
	"$viewer" 127.0.0.1 "$PORT" -i "$art/phone" -W 2304 -H 1440 -r 60,90 -B 60:20 -S "$secs" "$@" >"$art/viewer.log" 2>&1 &
	vpid=$!
	local i=0 name=
	while [ $i -lt 60 ]; do
		name=$(niri msg --json outputs 2>/dev/null | jq -r --arg s "$SERIAL" 'to_entries | map(select(.value.serial == $s)) | first | .key // empty')
		[ -n "$name" ] && break; sleep 0.2; i=$((i + 1)); done
	[ -n "$name" ] || fail "the virtual output never appeared"
	niri msg output "$name" scale 1 >/dev/null 2>&1
	"$pattern" -o "$name" -a -T -t $((secs + 10)) >"$art/pattern.log" 2>&1 &
	ppid=$!
	wait "$vpid"; vpid=
	kill "$ppid" 2>/dev/null; wait "$ppid" 2>/dev/null; ppid=
}

echo "-- 1. 90 Hz is not sustainable: the daemon steps down and the viewer follows"
run_viewer 62
grep -q "SETUP: 2304x1440@90" "$art/viewer.log" || fail "the stream did not start at 90 Hz"
grep -q "trying 60 Hz" "$art/daemon.log" || fail "the daemon never tried 60 Hz"
grep -q "RECONFIGURE" "$art/viewer.log" || fail "the viewer was not told to reconfigure"
echo "   ok: $(grep -m1 'trying 60 Hz' "$art/daemon.log" | sed 's/^\[[^]]*\] //')"

echo "-- 2. a healthy stretch at 60 Hz is remembered"
grep -q "60 Hz is stable for this device: remembering it" "$art/daemon.log" || fail "the stable rate was not remembered"
[ -s "$art/pc/learned_rates" ] || fail "no learned_rates file"
grep -q ' 60 90$' "$art/pc/learned_rates" || fail "unexpected learned_rates: $(cat "$art/pc/learned_rates")"
echo "   ok: $(cat "$art/pc/learned_rates")"
sleep 2 # the daemon ends the session when the viewer leaves

echo "-- 3. the next connection starts at 60 Hz"
run_viewer 8
grep -q "SETUP: 2304x1440@60" "$art/viewer.log" || fail "the second connection did not start at 60 Hz"
grep -q "sustains 60 Hz over the network (found earlier)" "$art/daemon.log" || fail "the daemon did not use the learned rate"
echo "   ok"

echo "-- 4. a new cap forgets it"
"$ctl" -s "$SOCK" set refresh_cap_hz 90 >/dev/null 2>&1 || "$ctl" --socket "$SOCK" set refresh_cap_hz 90 >/dev/null 2>&1 || fail "could not set the cap through the control socket"
[ ! -e "$art/pc/learned_rates" ] || fail "the learned rate survived a new cap"
echo "   ok"
echo "PASS: the daemon steps down to a sustainable rate, remembers it per device, starts there next time, and forgets it when the cap changes"
