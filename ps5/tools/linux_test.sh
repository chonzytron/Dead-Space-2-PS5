#!/usr/bin/env bash
# Dead Space 2 for PS5: the same program built for Linux, to test changes on a
# PC before they go on a console (ps5/BUILD.md, "On Linux").
#
#   ps5/tools/linux_test.sh build                 # configure (once) and compile
#   ps5/tools/linux_test.sh run <files> [flags]   # run it
#
# <files> is a folder with your own game files, laid out as the PS5 app's
# install/ folder (install/README.txt): the Disc 1 (and Disc 2) image and the
# Title Update #2 package. They are only read, never copied into the repository.
#
# The program finds everything next to itself, as on the PS5, so it runs from
# a folder of its own (DS2_LINUX_RUN, default ~/.local/share/ds2-ps5-linux):
# the program (a hard link to the build), install/ (a link to <files>),
# game/, UserData/ (saves, unpacked Severed, caches), logs/ and ds2.toml, which
# it keeps between runs. Flags
# after <files> go to the program (--name=value, as in ds2.toml).
#
# Compiled for the PS5's processor (-march=znver2: AMD Zen 2 or newer), as the
# console build is, so the program does what it does there.
#
# Environment: DS2_LINUX_BUILD (default out/linux), DS2_LINUX_JOBS (default:
# one per 2.5 GB of memory: the recompiled game's files are big to compile).
set -euo pipefail

repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
build=${DS2_LINUX_BUILD:-$repo/out/linux}
run_dir=${DS2_LINUX_RUN:-$HOME/.local/share/ds2-ps5-linux}

jobs() {
    if [[ -n ${DS2_LINUX_JOBS:-} ]]; then echo "$DS2_LINUX_JOBS"; return; fi
    local kb cpus n
    kb=$(awk '/MemTotal/ {print $2}' /proc/meminfo)
    cpus=$(nproc)
    n=$(( kb / 2600000 ))
    (( n < 1 )) && n=1
    (( n > cpus )) && n=$cpus
    echo "$n"
}

case ${1:-} in
build)
    [[ -f $repo/thirdparty/rexglue-sdk-ps5/include/rex/ps5_compat.h ]] || "$repo/ps5/tools/fetch_sdk.sh"
    if [[ ! -f $build/build.ninja ]]; then
        cmake -S "$repo/ps5" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
            -DDS2_PS5_LINUX=ON -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
            -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
            -DSDL_X11_XTEST=OFF \
            "-DCMAKE_C_FLAGS=-march=znver2" "-DCMAKE_CXX_FLAGS=-march=znver2"
    fi
    ninja -C "$build" -j "$(jobs)" ds2
    echo "==> built $build/ds2"
    ;;
run)
    files=${2:?"usage: $0 run <folder with your disc images and title update> [flags]"}
    shift 2
    [[ -x $build/ds2 ]] || { echo "no $build/ds2: run '$0 build' first" >&2; exit 2; }
    files=$(cd -- "$files" && pwd)
    case $files/ in "$repo"/*) echo "keep your game files outside the repository" >&2; exit 2 ;; esac
    mkdir -p "$run_dir"
    ln -f "$build/ds2" "$run_dir/ds2" 2>/dev/null || cp -f "$build/ds2" "$run_dir/ds2"
    if [[ -e $run_dir/install && ! -L $run_dir/install ]]; then
        echo "$run_dir/install is a folder, not a link: move it away first" >&2; exit 2
    fi
    ln -sfn "$files" "$run_dir/install"
    [[ -f $run_dir/ds2.toml ]] || cp "$repo/ps5/package/ds2.example.toml" "$run_dir/ds2.toml"
    echo "==> running from $run_dir (settings: $run_dir/ds2.toml, logs: $run_dir/logs)"
    cd "$run_dir"
    exec ./ds2 --user_data_root="$run_dir/UserData" "$@"
    ;;
*)
    sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
    ;;
esac
