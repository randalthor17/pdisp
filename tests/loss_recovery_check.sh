#!/bin/sh
# Reference invalidation: after the receiver loses frames, the encoder predicts from an older frame the receiver
# still has, so the picture is exactly right again without an IDR. When the loss is longer than the encoder's
# reference buffer it falls back to an IDR. Either way the damaged stream (lost frames removed) must decode
# bit-identically to the clean stream with the same frames skipped, with no decoder errors.
# Needs a VAAPI HEVC encoder and ffmpeg. Exits 77 (meson: skipped) when they are missing.
# usage: loss_recovery_check.sh <enc_synth> [artifact-dir]
set -u
synth=$1
art=${2:-$(mktemp -d)}
mkdir -p "$art"
skip() { echo "SKIP: $*"; exit 77; }
command -v ffmpeg >/dev/null 2>&1 || skip "ffmpeg not installed"
fail() { echo "FAIL: $*"; exit 1; }

run_case() { # name first last expected_ref
	name=$1; a=$2; b=$3; want=$4
	out="$art/$name.hevc"
	log=$("$synth" -n 240 -m -X "$a:$b" -o "$out" 2>&1)
	echo "$log" | grep -q 'encoder_open failed' && skip "no VAAPI HEVC encoder"
	ref=$(echo "$log" | grep -o '\[ref -[0-9]*\]' | head -1)
	[ "$ref" = "[ref -$want]" ] || fail "$name: expected the frame after the loss to predict $want frames back, got '$ref'"

	# the damaged stream: lost frames removed
	ffmpeg -v error -y -i "$out" -c copy -bsf:v "noise=drop=between(n\,$a\,$b)" "$art/$name.dmg.hevc" || fail "$name: could not cut the stream"
	errs=$(ffmpeg -v error -i "$art/$name.dmg.hevc" -f null - 2>&1 | head -3)
	[ -z "$errs" ] || fail "$name: decoder errors on the damaged stream: $errs"

	ffmpeg -v error -i "$out" -vf "select=not(between(n\,$a\,$b))" -f framemd5 - 2>/dev/null | grep -v '^#' | cut -d, -f6 > "$art/$name.clean.md5"
	ffmpeg -v error -i "$art/$name.dmg.hevc" -f framemd5 - 2>/dev/null | grep -v '^#' | cut -d, -f6 > "$art/$name.dmg.md5"
	n=$(wc -l < "$art/$name.clean.md5")
	[ "$n" -gt 200 ] || fail "$name: only $n frames decoded"
	cmp -s "$art/$name.clean.md5" "$art/$name.dmg.md5" || fail "$name: damaged stream does not decode identically to the clean one"
	echo "ok: $name (lost frames $a-$b, next frame predicts $want back, $n frames identical)"
}

# 4 lost frames: frame 99 is still held, so frame 104 predicts from it (5 back), no IDR
run_case short_loss 100 103 5
# 5 lost frames: the last usable reference is 6 back
run_case max_loss 100 104 6
# 6 or more: no reference left, the encoder falls back to an IDR
run_case long_loss 100 110 0

echo "PASS: loss recovery is bit-exact with reference invalidation and falls back to an IDR beyond the buffer"
