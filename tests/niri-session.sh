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
cfg=$(mktemp --suffix=.kdl)
trap 'rm -f "$cfg"' EXIT
cat >"$cfg" <<'KDL'
// a bare niri for the tests: no bar, no autostart, no animations
animations { off; }
hotkey-overlay { skip-at-startup; }
KDL
mem() {
	for c in /sys/class/drm/card*/device/mem_info_gtt_used; do
		[ -r "$c" ] && echo "$(basename "$(dirname "$(dirname "$c")")") gtt $(( $(cat "$c") / 1048576 )) MiB, vram $(( $(cat "${c/gtt/vram}") / 1048576 )) MiB"
	done
}
echo "GPU memory before:"; mem
res=$(mktemp)
niri -c "$cfg" -- sh -c "
	cd '$repo' && nix develop -c meson test -C build --print-errorlogs ${*:-} ; echo \$? >'$res'
	echo; echo 'GPU memory after the tests (still held by this niri):'; $(declare -f mem); mem
	niri msg action quit --skip-confirmation
"
echo; echo "niri has quit. GPU memory now:"; mem
rc=$(cat "$res" 2>/dev/null || echo 1); rm -f "$res"
echo "meson test exit status: $rc"
exit "$rc"
