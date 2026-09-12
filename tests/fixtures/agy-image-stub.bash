#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Hermetic agy image transport fixture. HOME must be a test-owned directory.
set -euo pipefail
input=$(cat)
printf '%s\n' "$input" > "$HOME/input"
printf '%s\n' "$@" > "$HOME/argv"
pwd -P > "$HOME/cwd"
# Real agy rejects contradictory effort flags, including the base client's
# medium default alongside the default gemini-3.7-flash-high model.
model=
effort=
while [[ $# -gt 0 ]]
do
    case $1 in
        --model) model=$2; shift ;;
        --effort) effort=$2; shift ;;
    esac
    shift
done
if [[ $model =~ -(low|medium|high)$ && -n $effort && $effort != "${BASH_REMATCH[1]}" ]]
then
    printf 'conflicting model and effort\n' >&2
    exit 1
fi
mode=${AI_TEST_IMAGE_MODE:-success}
if [[ $mode == sleep ]]
then
    exec sleep 10
fi
[[ $input =~ ai_glib_[a-f0-9_]+ ]]
image_name=${BASH_REMATCH[0]}
session=${image_name#ai_glib_}
session=${session//_/-}
brain="$HOME/.gemini/antigravity-cli/brain"
dir="$brain/$session"
mkdir -p "$dir"
artifact="$dir/${image_name}_123.jpg"
# Deliberately misleading extension: the reader must sniff PNG bytes.
printf '%s' 'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+aX1sAAAAASUVORK5CYII=' | base64 -d > "$artifact"
case $mode in
    missing) rm "$artifact" ;;
    nonimage) printf 'this is not raster image data' > "$artifact" ;;
    symlink) mv "$artifact" "$HOME/outside.png"; ln -s "$HOME/outside.png" "$artifact" ;;
    hardlink) ln "$artifact" "$HOME/outside.png" ;;
    fifo) rm "$artifact"; mkfifo "$artifact" ;;
    oversized) truncate -s 67108865 "$artifact" ;;
    stale) touch -t 200001010000 "$artifact" ;;
    dirlink) mv "$dir" "$HOME/outside-dir"; ln -s "$HOME/outside-dir" "$dir" ;;
    multiple) cp "$artifact" "$dir/${image_name}_456.png" ;;
    wrong-name) image_name=wrong_name ;;
esac
if [[ $mode == malformed ]]
then
    printf '%s\n' 'null' '{"event":"step_update","step_update":{"state":{},"tool_name":3,"tool_info":null}}' '{"event":"result","result":[]}'
    exit 0
fi
if [[ $mode == error ]]
then
    printf '{"event":"step_update","step_update":{"state":"ERROR","tool_name":"generate_image","tool_info":{"error":{"message":"backend refused image"}}}}\n'
else
    printf '{"event":"step_update","step_update":{"conversation_id":"%s","state":"DONE","tool_name":"generate_image","tool_info":{"parameters":{"ImageName":"%s"}}}}\n' "$session" "$image_name"
fi
if [[ $mode == noresult ]]
then
    exit 0
fi
if [[ $mode == mismatch ]]
then
    session=00000000-0000-0000-0000-000000000000
elif [[ $mode == traversal ]]
then
    session=../../outside
fi
status=SUCCESS
if [[ $mode == terminal-error ]]
then
    status=ERROR
fi
printf '{"event":"result","result":{"conversation_id":"%s","status":"%s","response":"An image is ready at /arbitrary/model/path.png"}}\n' "$session" "$status"
if [[ $mode == exit ]]
then
    printf 'child failed\n' >&2
    exit 1
fi
