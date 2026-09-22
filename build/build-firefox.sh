#!/bin/sh
# Cross-build Firefox ESR for armv7 softfp against the staged TV sysroot.
# Refuses to start unless the machine has 40 GB free. The object directory
# for this browser is larger than that, and a half-written tree is useless.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
TAG=$(cat "$ROOT/build/FIREFOX_TAG")
SRC=$ROOT/build/firefox-src
SYSROOT=$ROOT/sysroot
TC=${TC:-/Users/aicoder/toolchains/arm-webos-linux-gnueabi_sdk-buildroot}

avail_kb=$(df -k "$ROOT" | awk 'NR==2 {print $4}')
need_kb=$((40 * 1024 * 1024))
if [ "$avail_kb" -lt "$need_kb" ]; then
    echo "need 40 GB free on the volume that holds $ROOT, have $((avail_kb / 1024)) MB" >&2
    exit 1
fi

if [ ! -f "$SYSROOT/usr/include/stdio.h" ]; then
    echo "sysroot has no C headers. Stage runtime libs with scripts/stage-sysroot.sh, then add glibc 2.35 armv7 softfp headers under sysroot/usr/include." >&2
    exit 1
fi

if [ ! -d "$SRC/.git" ]; then
    git clone --depth 1 --branch "$TAG" https://github.com/mozilla-firefox/firefox.git "$SRC"
fi

export PATH="$TC/bin:$PATH"
export CC=arm-webos-linux-gnueabi-gcc
export CXX=arm-webos-linux-gnueabi-g++
export AR=arm-webos-linux-gnueabi-ar
export RANLIB=arm-webos-linux-gnueabi-ranlib
export STRIP=arm-webos-linux-gnueabi-strip
export PKG_CONFIG_SYSROOT_DIR=$SYSROOT
export PKG_CONFIG_LIBDIR=$SYSROOT/usr/lib/pkgconfig
export CFLAGS="-march=armv7-a -mfloat-abi=softfp -mfpu=neon --sysroot=$SYSROOT"
export CXXFLAGS="$CFLAGS"
export LDFLAGS="--sysroot=$SYSROOT"

cd "$SRC"
# Record the flag list this tag actually accepts. The mozconfig is the intent.
./mach configure --help >"$ROOT/build/configure-help.txt" || true
cp "$ROOT/build/mozconfig" "$SRC/mozconfig"

if ! command -v rustup >/dev/null 2>&1; then
    echo "rustup is required, with target armv7-unknown-linux-gnueabi" >&2
    exit 1
fi
rustup target add armv7-unknown-linux-gnueabi

./mach build

APP=$ROOT/app
rm -rf "$APP/firefox" "$APP/lib"
mkdir -p "$APP/lib"
# The install prefix layout is dist/bin after a successful build.
cp -a "$SRC/obj-webos/dist/bin/." "$APP/"
# Keep the smoke binary. mach's dist/bin uses the name firefox for the executable.
chmod 755 "$APP/geckotv.sh"
echo "built $TAG into $APP"
