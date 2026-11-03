#!/usr/bin/env bash
# Builds the release overlay: what Setup.bat lays over the game data it
# extracted from the user's disk image.
#
#   Plugins/x86_64/*.dll             the Windows plugins (plugins/apple, plugins/spfx)
#   Resources/unity default resources the Windows player's own resources (Unity)
#   _patches/<file>.gpatch           a binary patch for every converted game file
#   _patches/list.txt                the patched files, relative to GoS_Data
#
# usage: make_overlay.sh <original Data dir> <converted dir> <plugins dir> <unity default resources> <out.7z>
#   <converted dir> holds the converted files at their paths under Data
#   (globalgamemanagers, *.assets, Resources/unity_builtin_extra, StreamingAssets/ab/...).
# Needs g++ (builds the patch tool for this machine) and 7z.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ORIG="$1" CONV="$2" PLUGINS="$3" DEFAULTRES="$4" OUT="$(realpath -m "$5")"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

g++ -O2 -std=c++17 -o "$WORK/gosr_patch" "$HERE/gosr_patch.cpp"
mkdir -p "$WORK/overlay/Plugins/x86_64" "$WORK/overlay/Resources" "$WORK/overlay/_patches"
cp "$PLUGINS"/*.dll "$WORK/overlay/Plugins/x86_64/"
cp "$DEFAULTRES" "$WORK/overlay/Resources/unity default resources"

(cd "$CONV" && find . -type f | sed 's#^\./##' | sort) | while IFS= read -r f; do
    [ -f "$ORIG/$f" ] || { echo "no original for $f" >&2; exit 1; }
    mkdir -p "$WORK/overlay/_patches/$(dirname "$f")"
    "$WORK/gosr_patch" diff "$ORIG/$f" "$CONV/$f" "$WORK/overlay/_patches/$f.gpatch"
    # check the round trip before shipping it
    "$WORK/gosr_patch" apply "$ORIG/$f" "$WORK/overlay/_patches/$f.gpatch" "$WORK/check"
    cmp -s "$WORK/check" "$CONV/$f" || { echo "patch round trip failed for $f" >&2; exit 1; }
    printf '%s\r\n' "${f//\//\\}" >> "$WORK/overlay/_patches/list.txt"
done
rm -f "$OUT"
(cd "$WORK/overlay" && 7z a -mx=9 "$OUT" . >/dev/null)
echo "built $OUT"
