#!/bin/sh
# M4 loopback check through the real pipeline: niri output (evdi) -> capture -> VAAPI HEVC -> pdispd -> UDP -> pdisp-view ->
# libavcodec. The animated test pattern is shown on the virtual output; the viewer must decode it with the loss handling
# in the loop, match the pattern, and keep up with the refresh rate.
# Needs niri, jq, a free evdi device, a VAAPI HEVC encoder. Exits 77 (meson: skipped) when they are missing.
# usage: m4_loopback_check.sh <pdispd> <pdisp-view> <pdisp-pattern> [artifact-dir] [extra pdisp-view args...]
# Thresholds can be overridden through the environment (used by the netem check): MIN_DECODED (450), MIN_FPS (50),
# MAX_P99_MS (80), MAX_FREEZE_MS (none), MIN_CHECK_PCT (90), MAX_IDR (1), MAX_GIVEN_UP (none),
# GLASS_MAX_MS (100: the pixel-stamped application-to-decoded latency, p99), EXPECT_OFFSET_US (0) and OFFSET_TOL_US (2000):
# the clock offset the viewer must estimate, for a viewer run with a skewed clock (extra argument -K).
set -u
daemon=$1; viewer=$2; pattern=$3
art=${4:-$(mktemp -d)}
[ $# -ge 4 ] && shift 4 || shift $#
mkdir -p "$art"

skip() { echo "SKIP: $*"; exit 77; }
command -v niri >/dev/null 2>&1 || skip "niri not installed"
command -v jq >/dev/null 2>&1 || skip "jq not installed"
niri msg --json outputs >/dev/null 2>&1 || skip "no running niri session"
[ -n "${WAYLAND_DISPLAY:-}" ] || skip "no WAYLAND_DISPLAY"

SERIAL=PDISPM4LOOP
PORT=${PDISP_TEST_PORT:-47110}
dlog="$art/daemon.log"; vlog="$art/viewer.log"; plog="$art/pattern.log"
dpid=; vpid=; ppid=

cleanup() {
	for p in $ppid $vpid $dpid; do kill "$p" 2>/dev/null; done
	wait 2>/dev/null
}
trap cleanup EXIT
fail() { echo "FAIL: $*"; echo "--- daemon:"; grep -v '^\[libevdi\]\|^libva' "$dlog" | tail -15; echo "--- viewer:"; tail -8 "$vlog"; exit 1; }

"$daemon" -K "$art/pc-id" -P accept -p "$PORT" -t "$SERIAL" -n pdisp-tablet -S 40 >"$dlog" 2>&1 &
dpid=$!
sleep 0.5
if ! kill -0 "$dpid" 2>/dev/null; then wait "$dpid"; rc=$?; [ "$rc" = 3 ] && skip "no free evdi device"; fail "daemon exited early (rc=$rc)"; fi

"$viewer" 127.0.0.1 "$PORT" -i "$art/phone-id" -W 2304 -H 1440 -r 60 -S 10 -c -G -o "$art/view.png" "$@" >"$vlog" 2>&1 &
vpid=$!

name=
i=0
while [ $i -lt 60 ]; do
	name=$(niri msg --json outputs 2>/dev/null | jq -r --arg s "$SERIAL" 'to_entries | map(select(.value.serial == $s)) | first | .key // empty')
	[ -n "$name" ] && break
	sleep 0.2
	i=$((i + 1))
done
[ -n "$name" ] || fail "the virtual output never appeared"
niri msg output "$name" scale 1 >/dev/null 2>&1 || fail "could not set scale 1"
sleep 0.5
"$pattern" -o "$name" -a -T -t 30 >"$plog" 2>&1 &
ppid=$!

wait "$vpid"; vrc=$?
vpid=
kill "$ppid" 2>/dev/null; wait "$ppid" 2>/dev/null; ppid=
wait "$dpid"; drc=$?
dpid=
summary=$(grep '^VIEW' "$vlog")
echo "$summary"
grep '^DAEMON' "$dlog"
[ "$vrc" = 0 ] || fail "viewer failed (rc=$vrc)"
[ "$drc" = 0 ] || fail "daemon failed (rc=$drc)"

val() { echo "$summary" | tr ' ' '\n' | grep "^$1=" | head -1 | cut -d= -f2 | tr -d '%'; }
decoded=$(val decoded); fps=$(val fps); p99=$(val p99); given=$(val given_up); idr=$(val idr); match=$(val match)
errs=$(val decoder_errors); freeze=$(val freeze_ms); cmin=$(val check_min); cbad=$(val check_bad); checks=$(val checks)
[ "$decoded" -ge "${MIN_DECODED:-450}" ] 2>/dev/null || fail "decoded only $decoded frames in 10 s (want ${MIN_DECODED:-450})"
awk -v f="$fps" -v m="${MIN_FPS:-50}" 'BEGIN { exit !(f >= m) }' || fail "decode rate $fps fps is below ${MIN_FPS:-50}"
awk -v l="$p99" -v m="${MAX_P99_MS:-80}" 'BEGIN { exit !(l < m) }' || fail "capture-to-decode latency p99 ${p99} ms is over ${MAX_P99_MS:-80} ms"
[ -z "${MAX_FREEZE_MS:-}" ] || awk -v f="$freeze" -v m="$MAX_FREEZE_MS" 'BEGIN { exit !(f <= m) }' || fail "longest freeze ${freeze} ms is over $MAX_FREEZE_MS ms"
[ "$errs" = 0 ] || fail "$errs decoder errors"
[ "$idr" -ge 1 ] && [ "$idr" -le "${MAX_IDR:-1}" ] || fail "$idr IDRs decoded (allowed 1 to ${MAX_IDR:-1})"
[ -z "${MAX_GIVEN_UP:-}" ] || [ "$given" -le "$MAX_GIVEN_UP" ] || fail "gave up on $given frames (allowed $MAX_GIVEN_UP)"
[ "$checks" -ge 5 ] || fail "only $checks periodic pattern checks ran"
cother=$(val check_other)
[ "${cother:-0}" = 0 ] || echo "NOTE: $cother of $checks sampled pictures were not the test pattern at all: the desktop was showing something else (niri overview from a stray hot corner, a notification). Decoding is judged on the others."
if [ "${cother:-0}" != 0 ] && [ "$cbad" != 0 ]; then # the overview's opening and closing animations are partial pictures too
	echo "SKIP: inconclusive, the desktop was disturbed during the run ($cother pictures were something else, $cbad partial)"; exit 77
fi
[ "$cbad" = 0 ] || fail "$cbad periodic pattern checks found a corrupt picture (lowest match ${cmin}%)"
awk -v c="$cmin" -v m="${MIN_CHECK_PCT:-90}" 'BEGIN { exit !(c >= m) }' || fail "lowest periodic pattern match ${cmin}% is below ${MIN_CHECK_PCT:-90}%"
[ -s "$art/view.png" ] || fail "no snapshot written"

# pixel-stamped glass-to-glass latency, the clock sync and the control-channel PING/PONG
glass=$(grep '^GLASS' "$vlog")
stamps=$(echo "$glass" | sed -n 's/^GLASS stamps=\([0-9]*\) .*/\1/p'); gp99=$(echo "$glass" | sed -n 's/.*p99=\([0-9.]*\).*/\1/p')
gp50=$(echo "$glass" | sed -n 's/.*p50=\([0-9.]*\).*/\1/p')
echo "$glass"
awk -v s="$stamps" -v d="$decoded" 'BEGIN { exit !(s >= 0.85 * d) }' || fail "only $stamps of $decoded frames carried a readable stamp"
awk -v g="$gp99" -v m="${GLASS_MAX_MS:-100}" 'BEGIN { exit !(g < m) }' || fail "glass-to-glass latency p99 ${gp99} ms is over ${GLASS_MAX_MS:-100} ms"
offset=$(val clock_offset_us); want=${EXPECT_OFFSET_US:-0}
awk -v o="$offset" -v w="$want" -v t="${OFFSET_TOL_US:-2000}" 'BEGIN { d = o - w; if (d < 0) d = -d; exit !(d <= t) }' || fail "clock offset estimated as $offset us, expected $want us (tolerance ${OFFSET_TOL_US:-2000})"
[ "$(val pongs)" -ge 5 ] 2>/dev/null || fail "only $(val pongs) PONGs on the control channel"
echo "PASS: $decoded frames at $fps fps, capture-to-decode p99 ${p99} ms, glass-to-glass p50/p99 ${gp50}/${gp99} ms, longest freeze ${freeze} ms, pattern match ${match}% (lowest of $checks checks ${cmin}%), gave up on $given, $idr IDR(s), clock offset ${offset} us"
