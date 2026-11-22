#!/usr/bin/env bash
# Builds the project with the sanitizers and runs the tests that need no display (the unit tests). Two trees, because the thread sanitizer cannot be
# combined with the others:
#   build-asan   AddressSanitizer + UndefinedBehaviorSanitizer
#   build-tsan   ThreadSanitizer
# The display tests run under the same trees in a throwaway niri: tests/niri-session.sh (it uses both by default).
# usage: tests/sanitize.sh [asan|tsan]   (default: both)
set -u
cd "$(dirname "$0")/.."
trees=${1:-"asan tsan"}
rc=0
for t in $trees; do
	case $t in
		asan) dir=build-asan; san=address,undefined ;;
		tsan) dir=build-tsan; san=thread ;;
		*) echo "usage: $0 [asan|tsan]"; exit 2 ;;
	esac
	echo "== $t ($san)"
	{ [ -d "$dir" ] && meson setup --reconfigure "$dir" -Db_sanitize=$san -Db_lundef=false || meson setup "$dir" -Db_sanitize=$san -Db_lundef=false; } >/dev/null 2>&1 || { echo "cannot configure $dir"; rc=1; continue; }
	meson compile -C "$dir" >/dev/null 2>&1 || { meson compile -C "$dir" 2>&1 | grep -E "error|FAILED" -A3 | head -20; rc=1; continue; }
	# sanitized code is several times slower: give the tests more time
	out=$(mktemp)
	ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 UBSAN_OPTIONS=print_stacktrace=1 TSAN_OPTIONS="suppressions=$PWD/tests/tsan.supp" PDISP_TEST_SLOW=1 meson test -C "$dir" --no-suite integration --timeout-multiplier 6 --print-errorlogs >"$out" 2>&1 || rc=1
	grep -E "^ *[0-9]+/[0-9]+|^Ok:|^Fail:|^Timeout:|ERROR: |WARNING: |runtime error" "$out" | grep -v " OK "
	rm -f "$out"
done
exit $rc
