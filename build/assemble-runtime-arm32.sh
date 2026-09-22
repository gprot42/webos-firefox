#!/bin/bash
# Turn the 32-bit mach package into app/firefox-runtime-arm32.
# Libraries the TV must supply itself (glibc, libstdc++, the Mali GPU stack,
# PulseAudio, ALSA) are left out; everything else Firefox needs is bundled
# from the Debian armel sysroot so the GTK stack stays self-consistent.
# Run inside ffbuild:  podman exec ffbuild bash /src/build/assemble-runtime-arm32.sh
set -euo pipefail
SR=/work/sysroot-armel
OBJ=/work/obj-arm32
OUT=/src/app/firefox-runtime-arm32
TAR=$(ls "$OBJ"/dist/firefox-*.tar.xz | head -1)
# The TV's Mali driver needs GLIBCXX_3.4.29, newer than Debian 11's libstdc++,
# so libstdc++ and libgcc_s must come from the TV, as must the GPU stack.
TV_ONLY='^(ld-linux\.so\.3|libc\.so\.6|libm\.so\.6|libdl\.so\.2|libpthread\.so\.0|librt\.so\.1|libresolv\.so\.2|libutil\.so\.1|libstdc\+\+\.so\.6|libgcc_s\.so\.1|libEGL\.so\.1|libGLESv2\.so\.2|libGLdispatch\.so\.0|libGLX\.so\.0|libGL\.so\.1|libOpenGL\.so\.0|libwayland-egl\.so\.1|libwayland-server\.so\.0|libgbm\.so\.1|libdrm\.so\.2|libpulse.*|libasound\.so\.2)$'

echo "package: $TAR"
adapter=$(mktemp); cp "$OUT/libwayland-client.so.0" "$adapter"
rm -rf "$OUT" /tmp/ff32 && mkdir -p "$OUT" /tmp/ff32
tar -xJf "$TAR" -C /tmp/ff32 && cp -a /tmp/ff32/firefox/. "$OUT/"
cp "$adapter" "$OUT/libwayland-client.so.0"; rm -f "$adapter"
mkdir -p "$OUT/defaults/pref" && cp /src/app/defaults/pref/00-webos.js "$OUT/defaults/pref/"

needed() { readelf -d "$1" 2>/dev/null | awk -F'[][]' '/NEEDED/{print $2}'; }
for pass in 1 2 3 4 5 6; do
    added=0
    while IFS= read -r elf; do
        for lib in $(needed "$elf"); do
            [[ $lib =~ $TV_ONLY ]] && continue
            [ -e "$OUT/$lib" ] && continue
            src=$(ls "$SR"/lib/arm-linux-gnueabi/"$lib" "$SR"/usr/lib/arm-linux-gnueabi/"$lib" 2>/dev/null | head -1 || true)
            if [ -z "$src" ]; then echo "  unresolved: $lib (needed by $(basename "$elf"))"; continue; fi
            cp -L "$src" "$OUT/$lib"; added=$((added+1))
        done
    done < <(find "$OUT" -type f \( -name '*.so*' -o -name firefox -o -name firefox-bin \) -exec sh -c 'file -b "$1" | grep -q ELF && echo "$1"' _ {} \;)
    echo "pass $pass: bundled $added"
    [ "$added" -eq 0 ] && break
done
find "$OUT" -maxdepth 1 -name '*.so*' -newer "$TAR" -exec llvm-strip --strip-unneeded {} \; 2>/dev/null || true

echo "--- checks"
maxglibc=$(find "$OUT" -type f -exec sh -c 'file -b "$1" | grep -q ELF && objdump -T "$1" 2>/dev/null' _ {} \; | grep -oE 'GLIBC_[0-9.]+' | sort -Vu | tail -1)
maxcxx=$(find "$OUT" -type f -exec sh -c 'file -b "$1" | grep -q ELF && objdump -T "$1" 2>/dev/null' _ {} \; | grep -oE 'GLIBCXX_[0-9.]+' | sort -Vu | tail -1)
echo "newest glibc symbol needed: $maxglibc (TV has 2.35)"
echo "newest libstdc++ symbol needed: $maxcxx (TV provides at least 3.4.29)"
file "$OUT/firefox" | cut -d, -f1-3
du -sh "$OUT"
echo ASSEMBLE_ARM32_DONE
