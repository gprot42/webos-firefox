#!/bin/bash
# Turn a finished mach package into app/firefox-runtime and an IPK.
# Run inside the ffbuild container, after ./mach package.
set -euo pipefail

SRC=/src
WORK=/work
RUNTIME=$SRC/app/firefox-runtime
BRIDGE_LIB=/media/developer/apps/usr/palm/applications/org.webosbrew.bridge-64to32/lib
INTERP=$BRIDGE_LIB/ld-linux-aarch64.so.1
RPATH="\$ORIGIN:$BRIDGE_LIB"
TARBALL=$(echo "$WORK"/obj-firefox/dist/firefox-*.linux-aarch64.tar.xz)

echo "tarball $TARBALL"
rm -rf "$RUNTIME" "$WORK/firefox-unpack"
mkdir -p "$RUNTIME" "$WORK/firefox-unpack"
tar -xJf "$TARBALL" -C "$WORK/firefox-unpack"
cp -a "$WORK/firefox-unpack/firefox/." "$RUNTIME/"

copy_deps() {
    local bin=$1
    ldd "$bin" 2>/dev/null | awk '/=> \// {print $1, $3}' | while read -r soname lib; do
        [[ -n $lib && -f $lib ]] || continue
        case $soname in
            libc.so.6|libm.so.6|libdl.so.2|libpthread.so.0|librt.so.1|libresolv.so.2|libutil.so.1|ld-linux-aarch64.so.1)
                continue
                ;;
        esac
        cp -L "$lib" "$RUNTIME/$soname"
    done
}

for _ in 1 2 3 4 5 6; do
    while IFS= read -r bin; do
        copy_deps "$bin"
    done < <(find "$RUNTIME" -type f \( -name '*.so' -o -name 'firefox' -o -name 'firefox-bin' \))
done

while IFS= read -r bin; do
    file -b "$bin" | grep -q ELF || continue
    patchelf --set-rpath "$RPATH" "$bin" || true
    if patchelf --print-interpreter "$bin" >/dev/null 2>&1; then
        patchelf --set-interpreter "$INTERP" "$bin"
    fi
done < <(find "$RUNTIME" -type f)

# Firefox scans defaults/pref and defaults/preferences. It never reads
# distribution/preferences, which is only ever distribution.ini.
mkdir -p "$RUNTIME/defaults/pref"
cp "$SRC/app/defaults/pref/00-webos.js" "$RUNTIME/defaults/pref/"
chmod 755 "$RUNTIME/firefox"
echo "interpreter $(patchelf --print-interpreter "$RUNTIME/firefox")"
echo "rpath $(patchelf --print-rpath "$RUNTIME/firefox")"
ls -lh "$RUNTIME/firefox" "$RUNTIME/libxul.so" "$RUNTIME/libgtk-3.so.0"
du -sh "$RUNTIME"
python3 "$SRC/scripts/pack-ipk.py"
echo ASSEMBLE_DONE
