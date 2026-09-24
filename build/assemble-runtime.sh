#!/bin/bash
# Turn the mach package into app/firefox-runtime, then pack the IPK.
# Libraries the TV must supply itself (glibc, libstdc++, the Mali GPU stack,
# PulseAudio, ALSA) are left out; everything else Firefox needs is bundled
# from the Debian armel sysroot so the GTK stack stays self-consistent.
# Run inside ffbuild:  podman exec ffbuild bash /src/build/assemble-runtime.sh
#   TOOLCHAIN=nc4  use the buildroot-nc4 build (build/mozconfig-nc4) and its
#                  sysroot instead of the Debian armel one
#   OUT=<dir>      assemble somewhere other than app/firefox-runtime
#   PACK=0         do not pack an IPK (and so do not bump the version)
set -euo pipefail
case "${TOOLCHAIN:-arm32}" in
    nc4)
        SR=/work/nc4/out/host/arm-webos-linux-gnueabi/sysroot
        OBJ=/work/obj-nc4
        LIBDIRS="$SR/lib $SR/usr/lib"
        ADAPTER=/tmp/wayland-1.22.0/build-nc4/src/libwayland-client.so.0.22.0
        ADAPTER_HINT="CROSS=nc4 build/build-adapter.sh" ;;
    *)
        SR=/work/sysroot-armel
        OBJ=/work/obj-arm32
        LIBDIRS="$SR/lib/arm-linux-gnueabi $SR/usr/lib/arm-linux-gnueabi"
        ADAPTER=/tmp/wayland-1.22.0/build-arm32/src/libwayland-client.so.0.22.0
        ADAPTER_HINT="build/build-adapter.sh" ;;
esac
OUT=${OUT:-/src/app/firefox-runtime}
TAR=$(ls "$OBJ"/dist/firefox-*.tar.xz | head -1)
# libstdc++ is linked into Firefox statically; the TV's own copy stays on the
# list because the Mali driver loads it. libgcc_s and the GPU stack also come
# from the TV.
TV_ONLY='^(ld-linux\.so\.3|libc\.so\.6|libm\.so\.6|libdl\.so\.2|libpthread\.so\.0|librt\.so\.1|libresolv\.so\.2|libutil\.so\.1|libstdc\+\+\.so\.6|libgcc_s\.so\.1|libEGL\.so\.1|libGLESv2\.so\.2|libGLdispatch\.so\.0|libGLX\.so\.0|libGL\.so\.1|libOpenGL\.so\.0|libwayland-egl\.so\.1|libwayland-server\.so\.0|libgbm\.so\.1|libdrm\.so\.2|libpulse.*|libasound\.so\.2)$'

echo "package: $TAR"
# Always install the adapter fresh from its build tree. (Copying the old one
# aside through mktemp once left it mode 0600: unreadable to the jailed app
# user, so the loader silently fell back to the TV's libwayland-client.)
[ -f "$ADAPTER" ] || { echo "build the adapter first: $ADAPTER_HINT"; exit 1; }
rm -rf "$OUT" /tmp/ff32 && mkdir -p "$OUT" /tmp/ff32
tar -xJf "$TAR" -C /tmp/ff32 && cp -a /tmp/ff32/firefox/. "$OUT/"
install -m 0755 "$ADAPTER" "$OUT/libwayland-client.so.0"
mkdir -p "$OUT/defaults/pref" && cp /src/app/defaults/pref/00-webos.js "$OUT/defaults/pref/"

# Image decoders for GTK. nc4's gdk-pixbuf has PNG and JPEG built in, so it
# needs no loader modules or cache.
if [ "${TOOLCHAIN:-arm32}" != nc4 ]; then
# Debian's gdk-pixbuf loads PNG, JPEG and the rest as
# plug-ins listed in loaders.cache; without them GTK aborts on its first icon
# ("Failed to load image-missing.png: Unrecognized image file format"). The
# cache holds absolute paths on the TV; build/gdk-pixbuf-loaders.cache was
# generated there with the bundled gdk-pixbuf-query-loaders.
PB=$SR/usr/lib/arm-linux-gnueabi/gdk-pixbuf-2.0
mkdir -p "$OUT/gdk-pixbuf/loaders"
for l in png jpeg gif ico bmp xpm; do cp -L "$PB/2.10.0/loaders/libpixbufloader-$l.so" "$OUT/gdk-pixbuf/loaders/"; done
cp -L "$PB/gdk-pixbuf-query-loaders" "$OUT/gdk-pixbuf/"
[ -f /src/build/gdk-pixbuf-loaders.cache ] && cp /src/build/gdk-pixbuf-loaders.cache "$OUT/gdk-pixbuf/loaders.cache"
fi

needed() { readelf -d "$1" 2>/dev/null | awk -F'[][]' '/NEEDED/{print $2}'; }
for pass in 1 2 3 4 5 6; do
    added=0
    while IFS= read -r elf; do
        for lib in $(needed "$elf"); do
            [[ $lib =~ $TV_ONLY ]] && continue
            [ -e "$OUT/$lib" ] && continue
            src=$(for d in $LIBDIRS; do ls "$d/$lib" 2>/dev/null; done | head -1 || true)
            if [ -z "$src" ]; then echo "  unresolved: $lib (needed by $(basename "$elf"))"; continue; fi
            cp -L "$src" "$OUT/$lib"; added=$((added+1))
        done
    done < <(find "$OUT" -type f \( -name '*.so*' -o -name firefox -o -name firefox-bin \) -exec sh -c 'file -b "$1" | grep -q ELF && echo "$1"' _ {} \;)
    echo "pass $pass: bundled $added"
    [ "$added" -eq 0 ] && break
done
find "$OUT" -maxdepth 1 -name '*.so*' -newer "$TAR" -exec llvm-strip --strip-unneeded {} \; 2>/dev/null || true

# Do not patchelf these 32-bit binaries: setting RUNPATH with patchelf made
# every one of them segfault on start. The launcher's library path is used.

# The app runs as an unprivileged jail user: everything must be world-readable.
chmod -R a+rX "$OUT"
echo "--- checks"
unreadable=$(find "$OUT" -type f ! -perm -o=r | wc -l)
echo "files not readable by the jail user: $unreadable"
[ "$unreadable" -eq 0 ] || exit 1
maxglibc=$(find "$OUT" -type f -exec sh -c 'file -b "$1" | grep -q ELF && objdump -T "$1" 2>/dev/null' _ {} \; | grep -oE 'GLIBC_[0-9.]+' | sort -Vu | tail -1)
maxcxx=$(find "$OUT" -type f -exec sh -c 'file -b "$1" | grep -q ELF && objdump -T "$1" 2>/dev/null' _ {} \; | grep -oE 'GLIBCXX_[0-9.]+' | sort -Vu | tail -1 || true)
echo "newest glibc symbol needed: $maxglibc (TV has 2.35)"
# The nc4 build must load on glibc 2.12, bundled libraries included.
if [ "${TOOLCHAIN:-arm32}" = nc4 ]; then
    bash /src/build/nc4/glibc-check.sh "$OUT"
fi
# libstdc++ is linked statically (build/mozconfig-arm32), so nothing should
# need the TV's copy.
echo "newest libstdc++ symbol needed: ${maxcxx:-none, libstdc++ is static}"
file "$OUT/firefox" | cut -d, -f1-3
du -sh "$OUT"
if [ "${PACK:-1}" = 1 ]; then
    python3 /src/scripts/pack-ipk.py
fi
echo ASSEMBLE_DONE
