#!/bin/sh
# Copy the TV's glibc 2.35 runtime libraries into ./sysroot.
# The unpacked firmware has almost no usr/include. Firefox still needs a
# matching header tree. This script does not download or build one.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
FIRMWARE=${FIRMWARE:-/Users/aicoder/src/private/lg-webos-scripts/33.31.68.01-HE_DTV_W25G_AFABATAA}
ROOTFS=$FIRMWARE/rootfs.pak.unsquashfs
BSP=$FIRMWARE/bsppart.pak.unsquashfs/bsp
OUT=$ROOT/sysroot

if [ ! -d "$ROOTFS/lib" ] || [ ! -d "$BSP/usr/lib" ]; then
    echo "firmware tree not found under $FIRMWARE" >&2
    exit 1
fi

mkdir -p "$OUT/lib" "$OUT/usr/lib" "$OUT/usr/include"

copy_one() {
    src=$1
    dest=$2
    if [ -e "$src" ]; then
        cp -a "$src" "$dest/"
    else
        echo "missing $src" >&2
    fi
}

for name in ld-linux.so.3 libc.so.6 libm.so.6 libdl.so.2 libpthread.so.0 \
    librt.so.1 libresolv.so.2 libutil.so.1 libz.so.1 libz.so.1.2.11 libgcc_s.so.1; do
    copy_one "$ROOTFS/lib/$name" "$OUT/lib"
done

for pattern in libwayland-client.so libwayland-egl.so libwayland-cursor.so \
    libwayland-webos-client.so libpulse.so libasound.so libffi.so \
    libexpat.so libxkbcommon.so libstdc++.so; do
    found=0
    for src in "$ROOTFS/usr/lib/$pattern"*; do
        if [ -e "$src" ]; then
            cp -a "$src" "$OUT/usr/lib/"
            found=1
        fi
    done
    if [ "$found" -eq 0 ]; then
        echo "no match for $pattern" >&2
    fi
done

for pattern in libEGL.so libGLESv2.so libmali.so; do
    for src in "$BSP/usr/lib/$pattern"*; do
        if [ -e "$src" ]; then
            cp -a "$src" "$OUT/usr/lib/"
        fi
    done
done

echo "staged runtime libs in $OUT"
strings "$OUT/lib/libc.so.6" | grep "GNU C Library" | head -1
if [ ! -f "$OUT/usr/include/stdio.h" ]; then
    echo "headers are not in this sysroot. Firefox configure needs glibc 2.35 headers built for armv7 softfp." >&2
fi
