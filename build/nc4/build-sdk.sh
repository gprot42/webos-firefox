#!/bin/bash
# Build a buildroot-nc4 SDK with GTK 3 for compiling Firefox against glibc
# 2.12. Run inside ffbuild as builder:
#   podman exec -u builder ffbuild bash /src/build/nc4/build-sdk.sh
# Output: /work/nc4/out/images/arm-webos-linux-gnueabi_sdk-buildroot.tar.gz
# and the unpacked SDK in /work/nc4/out/host.
set -euo pipefail
NC4_REPO=https://github.com/openlgtv/buildroot-nc4.git
NC4_COMMIT=${NC4_COMMIT:-322ff04e}
BR=/work/nc4/br
OUT=/work/nc4/out
LOG=/work/nc4/build-sdk.log
exec > >(tee -a "$LOG") 2>&1

if [ ! -d "$BR/.git" ]; then
    git clone "$NC4_REPO" "$BR"
fi
git -C "$BR" fetch -q --depth 1 origin "$NC4_COMMIT" 2>/dev/null || true
git -C "$BR" checkout -q "$NC4_COMMIT"

# nc4 pins Wayland at 1.11.0, the version webOS 4 ships, because its other
# programs link against the TV's libwayland. GTK 3.24 needs 1.14.91 or newer.
# Firefox bundles its own libwayland-client (the adapter), so build against
# 1.18.0, the last release with autotools, which nc4's package uses. The
# checksum matches upstream Buildroot's package/wayland/wayland.hash.
sed -i -e 's/^WAYLAND_VERSION = .*/WAYLAND_VERSION = 1.18.0/' \
       -e 's|^WAYLAND_SITE = http://|WAYLAND_SITE = https://|' "$BR/package/wayland/wayland.mk"
grep -q 'wayland-1.18.0.tar.xz' "$BR/package/wayland/wayland.hash" ||
    echo 'sha256  4675a79f091020817a98fd0484e7208c8762242266967f55a67776936c2e294d  wayland-1.18.0.tar.xz' \
        >> "$BR/package/wayland/wayland.hash"
# Likewise wayland-protocols (XML only, nothing ships): GTK needs 1.17+; 1.20
# is the last autotools release. Checksum from upstream Buildroot.
sed -i -e 's/^WAYLAND_PROTOCOLS_VERSION = .*/WAYLAND_PROTOCOLS_VERSION = 1.20/' \
       -e 's|^WAYLAND_PROTOCOLS_SITE = http://|WAYLAND_PROTOCOLS_SITE = https://|' \
       "$BR/package/wayland-protocols/wayland-protocols.mk"
grep -q 'wayland-protocols-1.20.tar.xz' "$BR/package/wayland-protocols/wayland-protocols.hash" ||
    echo 'sha256  9782b7a1a863d82d7c92478497d13c758f52e7da4f197aa16443f73de77e4de7  wayland-protocols-1.20.tar.xz' \
        >> "$BR/package/wayland-protocols/wayland-protocols.hash"

make -C "$BR" O="$OUT" webos_tv_defconfig
"$BR"/support/kconfig/merge_config.sh -m -O "$OUT" "$OUT/.config" /src/build/nc4/firefox.fragment
make -C "$BR" O="$OUT" olddefconfig
for opt in LIBGTK3 LIBGTK3_WAYLAND PANGO CAIRO HARFBUZZ GDK_PIXBUF; do
    grep -q "^BR2_PACKAGE_$opt=y" "$OUT/.config" || { echo "option BR2_PACKAGE_$opt did not stick"; exit 1; }
done
grep -q '^BR2_TOOLCHAIN_HEADERS_AT_LEAST="3.17"' "$OUT/.config" || { echo "kernel headers are not 3.17"; exit 1; }
grep -q '^BR2_DEFAULT_KERNEL_HEADERS="3.17' "$OUT/.config" || { echo "kernel headers version is not 3.17.x"; exit 1; }
# Serial at the top level, as nc4 builds: glibc-polyfills is built before the
# toolchain is complete and does not work with per-package directories.
make -C "$BR" O="$OUT" sdk
echo SDK_DONE
