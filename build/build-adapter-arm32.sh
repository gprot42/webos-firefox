#!/bin/bash
# 32-bit libwayland-client with the webOS adapter, for the native softfp
# Firefox. Reuses the wayland tree that build-adapter.sh patches, with its own
# meson build directory and the cross file build/arm32-cross.ini.
# Run inside ffbuild:  podman exec ffbuild bash /src/build/build-adapter-arm32.sh
set -euo pipefail
WL=/tmp/wayland-1.22.0
OUT=${OUT:-/src/app/firefox-runtime-arm32}
# Prepare and patch the shared source tree exactly as the 64-bit build does,
# but skip its native compile: only the prep steps are wanted here.
[ -f "$WL/src/webos-xdg.c" ] || { echo "run build/build-adapter.sh once first"; exit 1; }
cp /src/src/webos-xdg.c "$WL/src/webos-xdg.c"
cd "$WL"
if [ ! -f build-arm32/build.ninja ]; then
    meson setup build-arm32 --cross-file /src/build/arm32-cross.ini \
        -Ddocumentation=false -Ddtd_validation=false -Dtests=false -Dscanner=false
fi
ninja -C build-arm32 src/libwayland-client.so.0.22.0
mkdir -p "$OUT"
cp -L build-arm32/src/libwayland-client.so.0.22.0 "$OUT/libwayland-client.so.0"
file "$OUT/libwayland-client.so.0"
echo ADAPTER_ARM32_OK
