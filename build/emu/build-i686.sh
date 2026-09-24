#!/bin/bash
# Build the 32-bit x86 Firefox for the webOS 4 emulator. Uses the same source
# tree and patches as the TV builds (build/linux-build.sh sets those up).
# Run inside ffbuild as root:  bash /src/build/emu/build-i686.sh
set -euo pipefail
if [[ "${1:-}" != "--as-user" ]]; then
    [[ -f /work/sysroot-i386/lib/i386-linux-gnu/libc.so.6 ]] || bash /src/build/emu/sysroot-i386.sh
    chown -R builder:builder /work/sysroot-i386
    exec su builder -s /bin/bash -c "bash /src/build/emu/build-i686.sh --as-user"
fi
export RUSTUP_HOME=/work/rustup CARGO_HOME=/work/cargo MOZBUILD_STATE_PATH=/work/mozbuild
export PATH="$CARGO_HOME/bin:$PATH"
export MOZCONFIG=/src/build/emu/mozconfig-i686
rustup target add i686-unknown-linux-gnu
cd /work/firefox
./mach configure
./mach build
./mach package
ls -l /work/obj-i686/dist/firefox-*.tar.xz
echo "BUILD_DONE $(date -Is)"
