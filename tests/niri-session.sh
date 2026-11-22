#!/usr/bin/env bash
# Runs the display tests (the ones that create virtual outputs: m3, m4, m5, m9, m10, m11 ...) in a throwaway niri of their own, so that the GPU memory
# niri keeps for every removed output (niri issue #1457) is freed when this niri exits, and your desktop session is never touched.
#
# A nested niri (a window in your session) cannot be used: it has no DRM backend, so it never sees evdi outputs. Instead:
#   1. Switch to a spare virtual terminal (Ctrl+Alt+F3) and log in.
#   2. cd to the repository and run:  tests/niri-session.sh [meson test arguments, e.g. m9 or --suite display]   (under the sanitizers, see below)
#   3. When it finishes it prints the result and the GPU memory before and after, quits its niri, and you can switch back (Ctrl+Alt+F2 or wherever
#      your session is) or log out. The memory the tests used goes with that niri.
# While it runs, your own session is in the background (no input, but it keeps running). Needs the same things as the tests: a free evdi device, VAAPI.
set -u
repo=$(cd "$(dirname "$0")/.." && pwd)
# Both sanitizer builds by default (AddressSanitizer + UBSan in build-asan, ThreadSanitizer in build-tsan, set up on first use, a few times slower).
# PDISP_BUILD_DIR=build runs the plain build instead (or any other build directory).
trees=${PDISP_BUILD_DIR:-"build-asan build-tsan"}
[ -n "${WAYLAND_DISPLAY:-}" ] && { echo "Run this from a plain terminal (a spare VT), not inside a desktop session: it starts its own niri."; exit 2; }
# The tests need the evdi device for themselves (there is one): pause your own pdisp services while they run, and start them again afterwards.
stopped=""
if [ "$(ls /sys/devices/platform 2>/dev/null | grep -c "^evdi\.")" -lt 2 ]; then # with a second evdi device (services.pdisp.evdiDevices = 2) the tests take that one and nothing needs pausing
for u in pdisp-gui pdispd; do systemctl --user is-active --quiet $u 2>/dev/null && { systemctl --user stop $u && stopped="$u $stopped"; }; done
fi
[ -n "$stopped" ] && echo "paused your services: $stopped(started again at the end)"
restore() { rm -f "$cfg"; for u in $stopped; do systemctl --user start $u; done; }
cfg=$(mktemp --suffix=.kdl)
trap restore EXIT
cat >"$cfg" <<'KDL'
// a bare niri for the tests: no bar, no autostart, no animations
animations { off; }
hotkey-overlay { skip-at-startup; }
// the checks that need the touchscreen mapped to the virtual output insert it into this block (tests/m8_*_check.sh)
input {
}
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
# The shared VRAM held by the niri that is already running (your desktop's): the throwaway niri frees its own on exit, but this one is the question.
main_niri=$(pgrep -x niri | head -1)
mainmem() { # MiB
	[ -n "$main_niri" ] || { echo "?"; return; }
	for f in /proc/"$main_niri"/fdinfo/*; do
		grep -q "drm-driver: amdgpu" "$f" 2>/dev/null && awk '/drm-shared-vram/ { if ($2 > m) m = $2 } END { print int(m / 1024) }' "$f"
	done | sort -n | tail -1
}
main_before=$(mainmem)
echo "GPU memory before:"; mem
echo "your desktop niri (pid ${main_niri:-none}) holds ${main_before:-?} MiB of shared VRAM"
args=; [ $# -gt 0 ] && args=$(printf '%q ' "$@") # test names with spaces survive the sh -c below; no arguments = every test
# safe here: this niri is a throwaway, so the checks that change its config or open windows may run
export PDISP_TEST_NIRI_CONFIG=1 PDISP_TEST_WINDOWS=1 PDISP_TEST_NIRI_REAL_CONFIG="$cfg" # the tests put this niri's own config back, not your desktop's
# the sanitizers abort the test that trips them; leaks at exit are not looked for (the daemon's child processes just exit)
export ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 TSAN_OPTIONS=halt_on_error=0

total=0
run_tree() { # run_tree <build directory>
	local builddir=$1 mult=1 san=
	case $builddir in build-asan) san=address,undefined; mult=6 ;; build-tsan) san=thread; mult=6 ;; esac # sanitized code is several times slower
	local res log="$repo/$builddir/niri-session-tests.log" # niri runs its command with stdout discarded, so meson's output goes to a file
	res=$(mktemp)
	if [ -n "$san" ] && [ ! -d "$repo/$builddir" ]; then (cd "$repo" && nix develop -c meson setup "$builddir" -Db_sanitize=$san -Db_lundef=false >/dev/null 2>&1); fi
	echo; echo "=================== tests in $builddir${san:+ (sanitizers: $san)}"
	mkdir -p "$repo/$builddir"
	if [ -n "$san" ]; then export PDISP_GUI_CLEAN_EXIT=1; else unset PDISP_GUI_CLEAN_EXIT; fi # the sanitizers should see the GUI shut down properly (see gui/main.cpp)
	niri -c "$cfg" -- sh -c "
		cd '$repo' && nix develop -c meson compile -C '$builddir' >'$log' 2>&1 && nix develop -c meson test -C '$builddir' --timeout-multiplier $mult --print-errorlogs $args >>'$log' 2>&1 ; echo \$? >'$res'
		echo; echo 'GPU memory after the tests (still held by this niri):' | tee -a '$res.mem'; $(declare -f mem); mem | tee -a '$res.mem'
		niri msg action quit --skip-confirmation
	"
	echo; echo "niri has quit. GPU memory now:"; mem
	echo "your desktop niri now holds $(mainmem) MiB of shared VRAM (before the tests: ${main_before:-?} MiB): the difference is what the tests cost it"
	local rc; rc=$(cat "$res" 2>/dev/null || echo 1); rm -f "$res"
	[ -r "$res.mem" ] && { echo "peak, from inside niri:"; cat "$res.mem"; rm -f "$res.mem"; }
	echo "meson output: $log"; tail -n 25 "$log" 2>/dev/null
	echo "sanitizer reports in the log: $(grep -c -E 'ERROR: (Address|Thread|Leak)Sanitizer|WARNING: ThreadSanitizer|runtime error' "$log" 2>/dev/null)"
	echo "meson test exit status ($builddir): $rc"
	[ "$rc" != 0 ] && total=1
}
for t in $trees; do run_tree "$t"; done
echo; echo "overall: $([ $total = 0 ] && echo all trees passed || echo FAILED)"
exit "$total"
