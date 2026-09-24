#!/bin/bash
# Assemble the 32-bit x86 Firefox for the webOS 4 emulator test, laid out like
# the TV app so the same launcher and paths apply:
#   /work/emu-app/{geckotv,firefox-runtime/...}  ->  /work/emu-app.tar.gz
# The emulator's glibc (2.24) is older than the build's (Debian 12, 2.36), so
# glibc and its loader are bundled and every executable is pointed at
# /tmp/ld32.so, which build/emu/run-firefox.sh links to the bundled loader.
# Run inside ffbuild as root, after build/emu/build-i686.sh and
# CROSS=i686 build/build-adapter.sh.
set -euo pipefail
SR=/work/sysroot-i386
APP=/work/emu-app
RT=$APP/firefox-runtime
TOOLCHAIN=i686 PACK=0 OUT=$RT bash /src/build/assemble-runtime.sh
for l in ld-linux.so.2 libc.so.6 libm.so.6 libdl.so.2 libpthread.so.0 librt.so.1 \
         libresolv.so.2 libutil.so.1 libgcc_s.so.1; do
    cp -L "$SR/lib/i386-linux-gnu/$l" "$RT/$l"
done
# Debian's gdk-pixbuf tells image types apart through the shared MIME
# database, which the emulator lacks (nc4's matches signatures itself): bundle
# a compiled one and point XDG_DATA_DIRS at it through the launcher's env file.
DEST=/media/developer/apps/usr/palm/applications/com.github.gprot42.geckotv
mkdir -p "$RT/share/mime/packages"
cp "$SR/usr/share/mime/packages/freedesktop.org.xml" "$RT/share/mime/packages/"
update-mime-database "$RT/share/mime"
echo "XDG_DATA_DIRS=$DEST/firefox-runtime/share" > "$APP/env"
clang-19 --target=i686-linux-gnu --sysroot="$SR" -fuse-ld=lld -O2 -Wall -std=c11 \
    -o "$APP/geckotv" /src/src/geckotv.c
exes=("$APP/geckotv")
while IFS= read -r f; do exes+=("$f"); done < <(
    find "$RT" -maxdepth 1 -type f -perm -u+x ! -name '*.so*' -exec sh -c \
        'file -b "$1" | grep -q "ELF 32-bit LSB.*executable" && echo "$1"' _ {} \;)
python3 /src/build/emu/set-interp.py /tmp/ld32.so "${exes[@]}"
chmod -R a+rX "$APP"
tar -C /work -czf /work/emu-app.tar.gz emu-app
ls -l /work/emu-app.tar.gz
echo EMU_ASSEMBLE_DONE
