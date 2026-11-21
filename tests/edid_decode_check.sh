#!/bin/sh
# Generate EDIDs for several modes and require `edid-decode --check` to accept all of them.
# usage: edid_decode_check.sh <pdisp-edid> <edid-decode>
set -eu
gen=$1
dec=$2
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT

fail=0
while read -r mode size; do
	f="$dir/${mode}.edid"
	"$gen" -m "$mode" -s "$size" -n pdisp-test -t PDISPTEST1 -o "$f"
	if ! out=$("$dec" --check "$f" 2>&1); then
		echo "edid-decode rejected $mode ($size mm):"
		echo "$out"
		fail=1
	else
		echo "ok: $mode $size mm"
	fi
done <<EOF
2400x1080@60 148x66
2304x1440@90 234x150
1920x1080@60 527x296
2560x1600@120 287x179
1080x2400@120 66x148
1080x2400@144 66x148
3840x2160@60 600x340
EOF
exit $fail
