#!/bin/sh
# M3 pass check against a running niri session and a VAAPI HEVC encoder:
#   the encoded file decodes without errors, ffprobe reports the expected stream (HEVC Main, size, 4:2:0,
#   BT.709 limited range tags, no B-frames, I then P only), mpv plays it, per-frame encode time is within budget,
#   and keyframes stay close to the largest P-frame.
# Needs niri, jq, ffprobe/ffmpeg and a free evdi device. Exits 77 (meson: skipped) when those are missing.
# usage: m3_encode_check.sh <pdisp-encode> <pdisp-pattern> [artifact-dir]
set -u
encode=$1
pattern=$2
art=${3:-$(mktemp -d)}
mkdir -p "$art"

skip() { echo "SKIP: $*"; exit 77; }
for t in niri jq ffprobe ffmpeg; do command -v "$t" >/dev/null 2>&1 || skip "$t not installed"; done
niri msg --json outputs >/dev/null 2>&1 || skip "no running niri session"
[ -n "${WAYLAND_DISPLAY:-}" ] || skip "no WAYLAND_DISPLAY"

SERIAL=PDISPM3CHK
W=2304; H=1440
out="$art/m3.hevc"
log="$art/encode.log"
pat_log="$art/pattern.log"
enc_pid=; pat_pid=

cleanup() {
	[ -n "$pat_pid" ] && kill "$pat_pid" 2>/dev/null
	[ -n "$enc_pid" ] && kill "$enc_pid" 2>/dev/null
	wait 2>/dev/null
}
trap cleanup EXIT
fail() { echo "FAIL: $*"; echo "--- encode log:"; cat "$log" 2>/dev/null | grep -v '^libva\|^\[libevdi\]'; exit 1; }

"$encode" -m ${W}x${H}@60 -s 234x150 -n pdisp-tablet -t "$SERIAL" -N 31 -o "$out" -S 8 -k 60 >"$log" 2>&1 &
enc_pid=$!

name=
i=0
while [ $i -lt 40 ]; do
	name=$(niri msg --json outputs 2>/dev/null | jq -r --arg s "$SERIAL" 'to_entries | map(select(.value.serial == $s)) | first | .key // empty')
	[ -n "$name" ] && break
	if ! kill -0 "$enc_pid" 2>/dev/null; then
		wait "$enc_pid"; rc=$?
		[ "$rc" = 3 ] && skip "no free evdi device"
		fail "encoder exited early (rc=$rc)"
	fi
	sleep 0.2
	i=$((i + 1))
done
[ -n "$name" ] || fail "output never appeared"
niri msg output "$name" scale 1 >/dev/null 2>&1 || fail "could not set scale 1"
sleep 0.5
"$pattern" -o "$name" -a -t 14 >"$pat_log" 2>&1 &
pat_pid=$!

wait "$enc_pid"; rc=$?
enc_pid=
grep -E '^(ENCODE|\[ *[0-9.]+\] encoder)' "$log"
[ "$rc" = 0 ] || fail "encoder failed (rc=$rc)"

# --- the stream
probe() { ffprobe -v error -select_streams v:0 -show_entries "stream=$1" -of default=nw=1:nk=1 "$out"; }
check() { [ "$(probe "$1")" = "$2" ] || fail "stream $1: expected '$2', got '$(probe "$1")'"; }
check codec_name hevc
check profile Main
check width $W
check height $H
check pix_fmt yuv420p
check has_b_frames 0
check color_range tv
check color_space bt709
check color_primaries bt709
check color_transfer iec61966-2-1

types=$(ffprobe -v error -select_streams v:0 -show_entries frame=pict_type -of csv=p=0 "$out" | sort | uniq -c | tr -s ' ' | tr '\n' ' ')
echo "frame types:$types"
echo "$types" | grep -q 'B' && fail "unexpected B-frames"
frames=$(ffprobe -v error -select_streams v:0 -show_entries frame=pict_type -of csv=p=0 "$out" | wc -l)
enc_frames=$(sed -n 's/^ENCODE frames=\([0-9]*\).*/\1/p' "$log")
[ "$frames" = "$enc_frames" ] || fail "ffprobe sees $frames frames, the encoder wrote $enc_frames"

errs=$(ffmpeg -v error -i "$out" -f null - 2>&1 | head -5)
[ -z "$errs" ] || fail "decode errors: $errs"
echo "decode: no errors ($frames frames)"

if command -v mpv >/dev/null 2>&1; then
	mpv --no-config --vo=null --ao=null --really-quiet --frames=120 "$out" >/dev/null 2>&1 || fail "mpv could not play the file"
	echo "mpv: plays"
else
	echo "mpv: not installed, skipped"
fi

# --- timing and keyframe size
ms=$(grep '^ENCODE_MS' "$log")
sizes=$(grep '^ENCODE_SIZE' "$log")
echo "$ms"; echo "$sizes"
val() { echo "$1" | tr ' ' '\n' | grep "^$2=" | head -1 | cut -d= -f2; }
# the line reads "upload avg=.. max=.. convert avg=.. max=.. encode avg=.. max=.. total_max=.."
enc_max=$(echo "$ms" | sed -n 's/.*encode avg=[0-9.]* max=\([0-9.]*\).*/\1/p')
tot_avg=$(echo "$ms" | awk '{for(i=1;i<=NF;i++){if($i ~ /^avg=/){split($i,a,"=");s+=a[2]}}} END{print s}')
awk -v m="$enc_max" 'BEGIN { exit !(m < 16.7) }' || fail "encode time max ${enc_max} ms exceeds one 60 Hz frame"
awk -v a="$tot_avg" 'BEGIN { exit !(a < 10) }' || fail "average upload+convert+encode ${tot_avg} ms is over budget"

i_max=$(echo "$sizes" | sed -n 's/.*I avg=[0-9]* max=\([0-9]*\).*/\1/p')
p_max=$(echo "$sizes" | sed -n 's/.*P avg=[0-9]* max=\([0-9]*\).*/\1/p')
awk -v i="$i_max" -v p="$p_max" 'BEGIN { exit !(i <= p * 1.25) }' || fail "largest keyframe $i_max bytes is more than 1.25x the largest P-frame $p_max"

echo "PASS: valid HEVC (BT.709 limited), no decode errors, encode avg ${tot_avg} ms/frame (max encode ${enc_max} ms), keyframe max ${i_max} B vs P max ${p_max} B"
