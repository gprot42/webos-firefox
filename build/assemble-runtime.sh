#!/bin/bash
# Turn the mach package into app/firefox-runtime, then pack the IPK.
# Libraries the TV must supply itself (glibc, libstdc++, the Mali GPU stack,
# PulseAudio, ALSA) are left out; everything else Firefox needs is bundled
# from the Debian armel sysroot so the GTK stack stays self-consistent.
# Run inside ffbuild:  podman exec ffbuild bash /src/build/assemble-runtime.sh
#   TOOLCHAIN=nc4  use the buildroot-nc4 build (build/mozconfig-nc4) and its
#                  sysroot instead of the Debian armel one; nc4-gcc for the
#                  GCC-compiled one (build/mozconfig-nc4-gcc)
#   OUT=<dir>      assemble somewhere other than app/firefox-runtime
#   PACK=0         do not pack an IPK (and so do not bump the version)
set -euo pipefail
case "${TOOLCHAIN:-arm32}" in
    nc4|nc4-gcc)
        SR=/work/nc4/out/host/arm-webos-linux-gnueabi/sysroot
        OBJ=/work/obj-${TOOLCHAIN}
        LIBDIRS="$SR/lib $SR/usr/lib"
        ADAPTER=/tmp/wayland-1.22.0/build-nc4/src/libwayland-client.so.0.22.0
        ADAPTER_HINT="CROSS=nc4 build/build-adapter.sh" ;;
    i686)
        # Only for testing on the webOS 4 emulator (build/emu/assemble-emu.sh).
        SR=/work/sysroot-i386
        OBJ=/work/obj-i686
        MULTIARCH=i386-linux-gnu
        LIBDIRS="$SR/lib/$MULTIARCH $SR/usr/lib/$MULTIARCH"
        ADAPTER=/tmp/wayland-1.22.0/build-i686/src/libwayland-client.so.0.22.0
        ADAPTER_HINT="CROSS=i686 build/build-adapter.sh" ;;
    *)
        SR=/work/sysroot-armel
        OBJ=/work/obj-arm32
        MULTIARCH=arm-linux-gnueabi
        LIBDIRS="$SR/lib/$MULTIARCH $SR/usr/lib/$MULTIARCH"
        ADAPTER=/tmp/wayland-1.22.0/build-arm32/src/libwayland-client.so.0.22.0
        ADAPTER_HINT="build/build-adapter.sh" ;;
esac
OUT=${OUT:-/src/app/firefox-runtime}
TAR=$(ls "$OBJ"/dist/firefox-*.tar.xz | head -1)
# The runtime comes from the package tarball, which only `mach package`
# refreshes: after a `mach build binaries` it still holds the old libxul (0.1.7
# and 0.1.8 shipped without a Firefox patch this way).
if [ "$OBJ/dist/bin/libxul.so" -nt "$TAR" ]; then
    echo "$TAR is older than $OBJ/dist/bin/libxul.so: run ./mach package first"
    exit 1
fi
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
# Debian 12's (the emulator test build) likewise.
case "${TOOLCHAIN:-arm32}" in nc4*|i686) ;; *)
# Debian's gdk-pixbuf loads PNG, JPEG and the rest as
# plug-ins listed in loaders.cache; without them GTK aborts on its first icon
# ("Failed to load image-missing.png: Unrecognized image file format"). The
# cache holds absolute paths on the TV; build/gdk-pixbuf-loaders.cache was
# generated there with the bundled gdk-pixbuf-query-loaders.
PB=$SR/usr/lib/$MULTIARCH/gdk-pixbuf-2.0
mkdir -p "$OUT/gdk-pixbuf/loaders"
for l in png jpeg gif ico bmp xpm; do cp -L "$PB/2.10.0/loaders/libpixbufloader-$l.so" "$OUT/gdk-pixbuf/loaders/"; done
cp -L "$PB/gdk-pixbuf-query-loaders" "$OUT/gdk-pixbuf/"
[ -f /src/build/gdk-pixbuf-loaders.cache ] && cp /src/build/gdk-pixbuf-loaders.cache "$OUT/gdk-pixbuf/loaders.cache"
;; esac

# GTK's Wayland input-method module, so GTK reports text-field focus to the
# adapter over text-input-v3 and the webOS keyboard opens for fields in pages.
# The cache holds the module's absolute path on the TV (generated there with
# gtk-query-immodules-3.0); the launcher points GTK_IM_MODULE_FILE at it.
case "${TOOLCHAIN:-arm32}" in
    nc4*) IMDIR=$SR/usr/lib/gtk-3.0/3.0.0/immodules ;;
    *) IMDIR=$SR/usr/lib/$MULTIARCH/gtk-3.0/3.0.0/immodules ;;
esac
# libwayland-egl.so.1 is left to the TV (its Mali driver is built against
# the TV's copy), but webOS 4 has none. Bundle our own where only the
# launcher adds it to the library path, and only when the TV lacks its own:
# it hands the calls to the GPU driver when the driver implements them, as
# older Mali drivers do with their own struct (src/wayland-egl-shim.c).
ARM_FLAGS="-march=armv7-a -mthumb -mfpu=neon -mfloat-abi=softfp"
case "${TOOLCHAIN:-arm32}" in
    nc4*) SHIM_CC="/work/nc4/out/host/bin/arm-webos-linux-gnueabi-gcc $ARM_FLAGS" ;;
    i686) SHIM_CC="clang-19 --target=i686-linux-gnu --sysroot=$SR -fuse-ld=lld" ;;
    *) SHIM_CC="clang-19 --target=arm-linux-gnueabi --sysroot=$SR -fuse-ld=lld $ARM_FLAGS" ;;
esac
mkdir -p "$OUT/fallback"
$SHIM_CC -O2 -Wall -fPIC -shared \
    -fvisibility=hidden -Wl,-soname,libwayland-egl.so.1 \
    -o "$OUT/fallback/libwayland-egl.so.1" /src/src/wayland-egl-shim.c -ldl
# getrandom() for TVs whose glibc lacks it (webOS 4); the launcher preloads it
# there, since Firefox's Rust code otherwise needs /dev/random, which webOS 4's
# app jail does not have.
$SHIM_CC -O2 -Wall -fPIC -shared -fvisibility=hidden \
    -o "$OUT/fallback/libgetrandom-compat.so" /src/src/getrandom-compat.c
# gpuprobe: walks the EGL/GLES path Firefox would take, step by step, to find
# out why a TV's GPU is unused (src/gpuprobe.c). Next to the launcher; finds
# the runtime's libwayland-client (the adapter) through its RUNPATH.
PROTO=$(mktemp -d)
wayland-scanner client-header /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml \
    "$PROTO/xdg-shell-client-protocol.h"
wayland-scanner private-code /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml \
    "$PROTO/xdg-shell-protocol.c"
# Wayland 1.22's headers, matching the adapter it links against (the sysroots
# have older ones, without wl_proxy_marshal_flags).
WLSRC=/tmp/wayland-1.22.0
$SHIM_CC -O2 -Wall -I"$PROTO" -I$WLSRC/src -I"$(dirname "$ADAPTER")" \
    -o "$(dirname "$OUT")/gpuprobe" /src/src/gpuprobe.c \
    "$PROTO/xdg-shell-protocol.c" "$ADAPTER" -Wl,-rpath,'$ORIGIN/firefox-runtime' -ldl
rm -rf "$PROTO"
# GLib/GTK runtime data, bundled so nothing depends on what the TV has:
#  - compiled GSettings schemas: GTK aborts if one it asks for is missing
#    (org.gtk.Settings.FileChooser when a page opens a file picker);
#  - an empty GIO module directory, so our GLib does not load the TV's GIO
#    plug-ins, built for another GLib (2.48 on webOS 4);
#  - xkeyboard-config data, for the default keymap xkbcommon builds when the
#    compositor's keymap is rejected (LG's has keycodes above 0xfff).
# The launcher points GSETTINGS_SCHEMA_DIR, GIO_MODULE_DIR and
# XKB_CONFIG_ROOT at these.
case "${TOOLCHAIN:-arm32}" in
    nc4*) COMPILE_SCHEMAS=/work/nc4/out/host/bin/glib-compile-schemas ;;
    *) COMPILE_SCHEMAS=glib-compile-schemas ;;
esac
mkdir -p "$OUT/glib-schemas" "$OUT/gio-modules"
"$COMPILE_SCHEMAS" --targetdir="$OUT/glib-schemas" "$SR/usr/share/glib-2.0/schemas"
[ -s "$OUT/glib-schemas/gschemas.compiled" ] || { echo "schema compilation failed"; exit 1; }
[ -d "$SR/usr/share/X11/xkb/symbols" ] || { echo "no xkeyboard-config data in the sysroot"; exit 1; }
mkdir -p "$OUT/xkb"
for d in compat keycodes rules symbols types; do cp -a "$SR/usr/share/X11/xkb/$d" "$OUT/xkb/"; done
mkdir -p "$OUT/gtk-immodules"
cp -L "$IMDIR/im-wayland.so" "$OUT/gtk-immodules/"
cp /src/build/gtk-immodules.cache "$OUT/gtk-immodules/immodules.cache"

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
case "${TOOLCHAIN:-arm32}" in
    nc4*) bash /src/build/nc4/glibc-check.sh "$OUT" ;;
esac
# libstdc++ is linked statically (build/mozconfig-arm32), so nothing should
# need the TV's copy.
echo "newest libstdc++ symbol needed: ${maxcxx:-none, libstdc++ is static}"
file "$OUT/firefox" | cut -d, -f1-3
du -sh "$OUT"
if [ "${PACK:-1}" = 1 ]; then
    python3 /src/scripts/pack-ipk.py
fi
echo ASSEMBLE_DONE
