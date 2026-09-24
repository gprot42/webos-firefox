#!/bin/bash
# Sysroot for the emulator test build: Debian 12 (bookworm) i386. LG's webOS
# TV 4.0 emulator is a 32-bit x86 system, so testing Firefox on its
# compositor needs a 32-bit x86 Firefox. GTK is 3.24 as in the nc4 build
# (3.24.38 here, 3.24.51 there). The emulator's glibc is 2.24, older than
# bookworm's 2.36, so the test build ships bookworm's glibc and its loader
# (build/emu/assemble-emu.sh). Packages are verified against
# debian-archive-keyring. Nothing from this build goes into the TV packages.
# Run inside the ffbuild container as root:  bash /src/build/emu/sysroot-i386.sh
set -euo pipefail
SR=${SR:-/work/sysroot-i386}
export DEBIAN_FRONTEND=noninteractive
PKGS=libc6-dev,libstdc++-12-dev,libgcc-12-dev,linux-libc-dev,libgtk-3-dev,libglib2.0-dev,libpango1.0-dev,libcairo2-dev,libgdk-pixbuf-2.0-dev,libatk1.0-dev,libfontconfig-dev,libfreetype-dev,libwayland-dev,libxkbcommon-dev,libdrm-dev,libgbm-dev,libegl-dev,libgles-dev,libpulse-dev,libasound2-dev,libdbus-1-dev,libffi-dev,libx11-xcb-dev,libxrandr-dev,libxtst-dev,libxcomposite-dev,libxdamage-dev,libxfixes-dev,libxext-dev,libxt-dev,xkb-data,gsettings-desktop-schemas
rm -rf "$SR"
mmdebstrap --mode=fakechroot --variant=extract --architectures=i386 \
    --keyring=/usr/share/keyrings/debian-archive-keyring.gpg \
    --include="$PKGS" bookworm "$SR" http://deb.debian.org/debian
python3 /src/build/sysroot-relink.py "$SR"
strings "$SR/lib/i386-linux-gnu/libc.so.6" | grep -m1 "GNU C Library"
echo "SYSROOT_DONE $SR"
