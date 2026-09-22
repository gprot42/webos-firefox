#!/bin/bash
# Native 32-bit (ARMv7, NEON, softfp) Firefox ESR 153 for the webOS TV.
# Prerequisites: the ffbuild container with the 64-bit build's tree at
# /work/firefox and toolchains from build/linux-build.sh, plus the sysroot
# from build/sysroot-armel.sh.
# Run as builder:  podman exec -u builder ffbuild bash /src/build/linux-build-arm32.sh
set -euo pipefail
export RUSTUP_HOME=/work/rustup CARGO_HOME=/work/cargo PATH=/work/cargo/bin:$PATH
export MOZBUILD_STATE_PATH=/work/mozbuild MOZCONFIG=/src/build/mozconfig-arm32
LOG=/work/linux-build-arm32.log
exec > >(tee -a "$LOG") 2>&1
phase() { echo; echo "===== $(date -Is) $* ====="; }

[ -f /work/sysroot-armel/lib/arm-linux-gnueabi/libc.so.6 ] || { echo "run build/sysroot-armel.sh first"; exit 1; }
rustup target add armv7-unknown-linux-gnueabi >/dev/null

phase "patch firefox build system"
cd /work/firefox
if git -c safe.directory=/work/firefox apply --check /src/build/patches/rust-target-softfp.patch 2>/dev/null; then
    git -c safe.directory=/work/firefox apply /src/build/patches/rust-target-softfp.patch && echo applied
else
    echo "already applied"
fi

phase "configure"
./mach configure
phase "build"
./mach build
phase "package"
./mach package
ls -l /work/obj-arm32/dist/firefox-*.tar.xz
echo "BUILD_ARM32_DONE $(date -Is)"
