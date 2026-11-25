#!/usr/bin/env bash
# Dead Space 2 for PS5: a zip of only the repository files the PS5 release is
# built from, ready to unpack and build with ps5/tools/build_ps5.sh.
#
#   ps5/tools/make_source_zip.sh [out.zip]     (default out/DeadSpace2-PS5-src-<version>.zip)
#
# In it: the recompiled game (src_tu2/, less the generator's metadata), the
# disc-swap code shared with the PC app, the SDK pin and patches, ps5/ less the
# tools that only check, debug or make art, and the cloud-session permission
# rule. Left out: the PC app, launcher, installer, mods, profiles, PC tools,
# docs and test scripts.
#
# The src_tu2 list was checked against the compiler's dependency record of a
# full PS5 build: every file the build opened is in, and only the five
# metadata files below are left out of src_tu2.
set -euo pipefail

repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
version=$(head -1 "$repo/ps5/VERSION.txt")
out=${1:-$repo/out/DeadSpace2-PS5-src-$version.zip}
command -v zip > /dev/null || { echo "needs zip (apt install zip)" >&2; exit 2; }
git -C "$repo" rev-parse --git-dir > /dev/null 2>&1 || { echo "$repo is not a git checkout" >&2; exit 2; }

list=$(mktemp)
trap 'rm -f "$list"' EXIT
{
    # The generated tree, as tracked, without what only the generator and the
    # PC tools read.
    git -C "$repo" ls-files src_tu2 |
        grep -vxE 'src_tu2/(symbols\.tsv|manifest\.tsv|inputs\.sha256|source\.fingerprint|ds2_names\.h)'
    printf '%s\n' \
        src/app/ds2_disc_swap.cpp src/app/ds2_disc_swap.h \
        sdk/REXGLUE_BASE.txt sdk/rexglue_ds2.patch sdk/rexglue_ps5.patch sdk/ffmpeg_ps5.patch \
        ps5/BUILD.md ps5/CMakeLists.txt ps5/VERSION.txt \
        ps5/tools/build_ps5.sh ps5/tools/fetch_sdk.sh ps5/tools/ps5_package.sh \
        ps5/tools/ps5_skip_link.sh ps5/tools/ps5_emutls.c ps5/tools/ps5_stat.c \
        ps5/tools/ps5_relro_pad.c \
        .claude/settings.json
    # The app, the CMake helpers and the package assets, as tracked (the Linux
    # test build's two files come along: ps5/CMakeLists.txt names them).
    git -C "$repo" ls-files ps5/src ps5/cmake ps5/package
} > "$list"

missing=0
while IFS= read -r f; do
    [[ -f $repo/$f ]] || { echo "missing: $f" >&2; missing=1; }
done < "$list"
(( missing == 0 )) || exit 1

mkdir -p "$(dirname "$out")"
out=$(realpath "$out")
rm -f -- "$out"
(cd "$repo" && zip -q -X -@ "$out" < "$list")
echo "$out: $(wc -l < "$list") files, $(du -h "$out" | cut -f1)"
