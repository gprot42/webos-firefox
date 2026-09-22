#!/bin/bash
# Build inside ffbuild:  podman exec ffbuild bash /src/experiments/printf-trace/build.sh
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p out
clang-19 --target=arm-linux-gnueabi --sysroot=/work/sysroot-armel -march=armv7-a -mthumb \
    -mfpu=neon -mfloat-abi=softfp -fuse-ld=lld -O2 -fPIC -shared -Wall -Wextra \
    -o out/libprintf-trace.so printf-trace.c -ldl
file out/libprintf-trace.so
