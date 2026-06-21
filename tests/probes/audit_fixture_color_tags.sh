#!/usr/bin/env bash
# Phase-0 probe P0.6: audit whether fixtures produced by the e2e ffmpeg recipe
# carry color tags. Prints per-stream color metadata and classifies the fixture
# as TAGGED or UNTAGGED, so color-metadata's Phase-1 default-tagging can be
# proven a no-op against today's height>576 heuristic.
set -euo pipefail

FFMPEG=${FFMPEG:-/opt/homebrew/bin/ffmpeg}
FFPROBE=${FFPROBE:-/opt/homebrew/bin/ffprobe}

command -v "$FFMPEG" >/dev/null || { echo "SKIP: no ffmpeg"; exit 0; }
command -v "$FFPROBE" >/dev/null || { echo "SKIP: no ffprobe"; exit 0; }

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
out="$tmp/fixture.mkv"

# Mirror tests/e2e/run_playback_e2e.sh's untagged recipe: testsrc2 + yuv420p,
# no -color_* flags.
"$FFMPEG" -hide_banner -loglevel error \
    -f lavfi -i "testsrc2=size=1280x720:rate=30:duration=1" \
    -c:v mpeg2video -pix_fmt yuv420p -g 30 -b:v 4M "$out"

metadata=$("$FFPROBE" -v error -select_streams v:0 \
    -show_entries stream=color_space,color_primaries,color_transfer,color_range \
    -of default=nw=1 "$out")

cs=unset
cp=unset
ct=unset
cr=unset
while IFS='=' read -r key value; do
    case "$key" in
        color_space) cs=${value:-unset} ;;
        color_primaries) cp=${value:-unset} ;;
        color_transfer) ct=${value:-unset} ;;
        color_range) cr=${value:-unset} ;;
    esac
done <<< "$metadata"

echo "color_space=${cs:-unset} color_primaries=${cp:-unset} color_transfer=${ct:-unset} color_range=${cr:-unset}"

if [ "${cs:-unknown}" = "unknown" ] || [ "${cs:-unset}" = "unset" ] || [ -z "${cs:-}" ]; then
    echo "CLASSIFICATION: UNTAGGED"
    echo "POLICY: untagged 720p fixture defaults to BT709/video (matches qtpreviewsink height>576)"
else
    echo "CLASSIFICATION: TAGGED ($cs)"
fi
