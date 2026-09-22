#!/bin/bash
# Build inside ffbuild:  podman exec ffbuild bash /src/experiments/egl-trace/build.sh
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p out
clang-19 --target=arm-linux-gnueabi --sysroot=/work/sysroot-armel -march=armv7-a -mthumb \
    -mfpu=neon -mfloat-abi=softfp -fuse-ld=lld -O2 -fPIC -shared -Wall -Wextra \
    -Wl,-soname,libEGL.so.1 -o out/libEGL.so.1 egl-trace.c -ldl
file out/libEGL.so.1
