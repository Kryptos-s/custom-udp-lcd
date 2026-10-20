#!/usr/bin/env bash
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
case ${1:-native} in
    native)
        cmake -S "$root" -B "$root/build/native" -G Ninja -DCMAKE_BUILD_TYPE=Release
        cmake --build "$root/build/native" --parallel
        ;;
    aarch64)
        docker build -f "$root/Dockerfile.cross" -t udm-lcd-cross "$root"
        docker run --rm --user "$(id -u):$(id -g)" -v "$root:/work" udm-lcd-cross sh -c 'cmake -S /work -B /work/build/aarch64 -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=/work/cmake/aarch64.cmake && cmake --build /work/build/aarch64 --parallel'
        ;;
    *) printf 'Usage: %s [native|aarch64]\n' "$0" >&2; exit 2 ;;
esac
