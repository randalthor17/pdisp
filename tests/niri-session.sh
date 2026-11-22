#!/usr/bin/env bash
# Runs the display tests (the ones that create virtual outputs: m3, m4, m5, m9, m10, m11 ...) in a throwaway niri of their own, so that the GPU memory
# niri keeps for every removed output (niri issue #1457) is freed when this niri exits, and your desktop session is never touched.
#
# A nested niri (a window in your session) cannot be used: it has no DRM backend, so it never sees evdi outputs. Instead:
#   1. Switch to a spare virtual terminal (Ctrl+Alt+F3) and log in.
#   2. cd to the repository and run:  tests/niri-session.sh [meson test arguments, e.g. m9 or --suite display]
#   3. When it finishes it prints the result and the GPU memory before and after, quits its niri, and you can switch back (Ctrl+Alt+F2 or wherever
#      your session is) or log out. The memory the tests used goes with that niri.
# While it runs, your own session is in the background (no input, but it keeps running). Needs the same things as the tests: a free evdi device, VAAPI.
set -u
repo=$(cd "$(dirname "$0")/.." && pwd)
[ -n "${WAYLAND_DISPLAY:-}" ] && { echo "Run this from a plain terminal (a spare VT), not inside a desktop session: it starts its own niri."; exit 2; }
# The tests need the evdi device for themselves (there is one): pause your own pdisp services while they run, and start them again afterwards.
stopped=""
for u in pdisp-gui pdispd; do systemctl --user is-active --quiet $u 2>/dev/null && { systemctl --user stop $u && stopped="$u $stopped"; }; done
[ -n "$stopped" ] && echo "paused your services: $stopped(started again at the end)"
restore() { rm -f "$cfg"; for u in $stopped; do systemctl --user start $u; done; }
cfg=$(mktemp --suffix=.kdl)
trap restore EXIT
cat >"$cfg" <<'KDL'
// a bare niri for the tests: no bar, no autostart, no animations
animations { off; }
hotkey-overlay { skip-at-startup; }
// the virtual outputs of the tests (evdi connectors) at scale 1, as in a normal setup: a phone-sized output would otherwise get scale 2
output "DVI-I-1" { scale 1; }
output "DVI-I-2" { scale 1; }
output "DVI-I-3" { scale 1; }
output "DVI-I-4" { scale 1; }
KDL
mem() {
	for c in /sys/class/drm/card*/device/mem_info_gtt_used; do
		[ -r "$c" ] && echo "$(basename "$(dirname "$(dirname "$c")")") gtt $(( $(cat "$c") / 1048576 )) MiB, vram $(( $(cat "${c/gtt/vram}") / 1048576 )) MiB"
	done
}
echo "GPU memory before:"; mem
args=$(printf '%q ' "$@") # test names with spaces survive the sh -c below
res=$(mktemp)
log="$repo/build/niri-session-tests.log" # niri runs its command with stdout discarded, so meson's output goes to a file
# safe here: this niri is a throwaway, so the checks that change its config or open windows may run
export PDISP_TEST_NIRI_CONFIG=1 PDISP_TEST_WINDOWS=1 PDISP_TEST_NIRI_REAL_CONFIG="$cfg" # the tests put this niri's own config back, not your desktop's
niri -c "$cfg" -- sh -c "
	cd '$repo' && nix develop -c meson test -C build --print-errorlogs $args >'$log' 2>&1 ; echo \$? >'$res'
	echo; echo 'GPU memory after the tests (still held by this niri):' | tee -a '$res.mem'; $(declare -f mem); mem | tee -a '$res.mem'
	niri msg action quit --skip-confirmation
"
echo; echo "niri has quit. GPU memory now:"; mem
rc=$(cat "$res" 2>/dev/null || echo 1); rm -f "$res"
[ -r "$res.mem" ] && { echo "peak, from inside niri:"; cat "$res.mem"; rm -f "$res.mem"; }
echo "meson output: $log"; tail -n 25 "$log" 2>/dev/null
echo "meson test exit status: $rc"
exit "$rc"
