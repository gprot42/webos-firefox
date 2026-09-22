#!/bin/bash
# Build the 32-bit sysroot for the native webOS Firefox: Debian 11 (bullseye)
# armel. armel uses the soft-float calling convention, which is
# call-compatible with the TV's softfp userspace, and bullseye's glibc 2.31 is
# older than the TV's 2.35, so nothing linked against it needs a newer glibc.
# Packages are verified against debian-archive-keyring.
# Run inside the ffbuild container as root:  bash /src/build/sysroot-armel.sh
set -euo pipefail
SR=${SR:-/work/sysroot-armel}
export DEBIAN_FRONTEND=noninteractive
apt-get install -y --no-install-recommends mmdebstrap symlinks fakechroot fakeroot debian-archive-keyring >/dev/null
PKGS=libc6-dev,libstdc++-10-dev,libgcc-10-dev,linux-libc-dev,libgtk-3-dev,libglib2.0-dev,libpango1.0-dev,libcairo2-dev,libgdk-pixbuf-2.0-dev,libatk1.0-dev,libfontconfig-dev,libfreetype-dev,libwayland-dev,libxkbcommon-dev,libdrm-dev,libgbm-dev,libegl-dev,libgles-dev,libpulse-dev,libasound2-dev,libdbus-1-dev,libffi-dev,libx11-xcb-dev,libxrandr-dev,libxtst-dev,libxcomposite-dev,libxdamage-dev,libxfixes-dev,libxext-dev,libxt-dev
rm -rf "$SR"
mmdebstrap --mode=fakechroot --variant=extract --architectures=armel \
    --keyring=/usr/share/keyrings/debian-archive-keyring.gpg \
    --aptopt='Acquire::Check-Valid-Until "false"' \
    --include="$PKGS" bullseye "$SR" http://archive.debian.org/debian
python3 /src/build/sysroot-relink.py "$SR"

# C++: Debian 11's GCC 10 headers are too old for Firefox (std::lerp with
# mixed float/double arguments is ambiguous there). The TV runs GCC 11's
# libstdc++ (GLIBCXX_3.4.29), so use GCC 11's headers and compiler support
# files from Debian 12, verified like the rest, and link against the TV's own
# libstdc++, staged from the firmware by scripts/stage-sysroot.sh. Clang picks
# the newest GCC it finds in the sysroot.
G11=$(mktemp -d)
mmdebstrap --mode=fakechroot --variant=extract --architectures=armel \
    --keyring=/usr/share/keyrings/debian-archive-keyring.gpg \
    --include=libstdc++-11-dev,libgcc-11-dev bookworm "$G11" http://deb.debian.org/debian
cp -a "$G11"/usr/include/c++/11 "$SR"/usr/include/c++/
cp -a "$G11"/usr/include/arm-linux-gnueabi/c++/11 "$SR"/usr/include/arm-linux-gnueabi/c++/
cp -a "$G11"/usr/lib/gcc/arm-linux-gnueabi/11 "$SR"/usr/lib/gcc/arm-linux-gnueabi/
rm -rf "$G11"
TVLIB=/src/sysroot/usr/lib/libstdc++.so.6.0.29
[ -f "$TVLIB" ] || { echo "missing $TVLIB: run scripts/stage-sysroot.sh on the Mac"; exit 1; }
cp "$TVLIB" "$SR"/usr/lib/arm-linux-gnueabi/libstdc++.so.6.0.29
ln -sf libstdc++.so.6.0.29 "$SR"/usr/lib/arm-linux-gnueabi/libstdc++.so.6
python3 /src/build/sysroot-relink.py "$SR"
strings "$SR/lib/arm-linux-gnueabi/libc.so.6" | grep -m1 "GNU C Library"
echo "SYSROOT_DONE $SR"
