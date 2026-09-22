#!/bin/sh
# Cross-build a Wayland-only GTK3 into app/lib before the Firefox compile.
# Stops before downloading sources unless the sysroot has headers and the
# volume has 8 GB free. GTK's own dependencies (glib, cairo, pango,
# gdk-pixbuf, harfbuzz, libxkbcommon) are not on the TV.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
SYSROOT=$ROOT/sysroot
PREFIX=$ROOT/build/gtk-install
WORK=$ROOT/build/gtk-build
TC=${TC:-/Users/aicoder/toolchains/arm-webos-linux-gnueabi_sdk-buildroot}

avail_kb=$(df -k "$ROOT" | awk 'NR==2 {print $4}')
need_kb=$((8 * 1024 * 1024))
if [ "$avail_kb" -lt "$need_kb" ]; then
    echo "need 8 GB free to build GTK, have $((avail_kb / 1024)) MB" >&2
    exit 1
fi

if [ ! -f "$SYSROOT/usr/include/stdio.h" ]; then
    echo "sysroot/usr/include is empty. GTK has to be configured against glibc 2.35 headers, not the SDK's glibc 2.12 headers." >&2
    exit 1
fi

export PATH="$TC/bin:$PATH"
export CC=arm-webos-linux-gnueabi-gcc
export CXX=arm-webos-linux-gnueabi-g++
export PKG_CONFIG_SYSROOT_DIR=$SYSROOT
export PKG_CONFIG_LIBDIR=$PREFIX/lib/pkgconfig:$SYSROOT/usr/lib/pkgconfig
export CFLAGS="-march=armv7-a -mfloat-abi=softfp -mfpu=neon --sysroot=$SYSROOT -I$PREFIX/include"
export LDFLAGS="--sysroot=$SYSROOT -L$PREFIX/lib"

mkdir -p "$WORK" "$PREFIX"
echo "preflight passed, but the GTK dependency build is not encoded yet" >&2
echo "next step is glib, cairo, pango, gdk-pixbuf, harfbuzz, libxkbcommon, then GTK 3" >&2
echo "host triple arm-webos-linux-gnueabi, prefix $PREFIX, wayland on, x11 off" >&2
exit 2
