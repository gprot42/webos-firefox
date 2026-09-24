#!/bin/sh
# Start the emulator test build of Firefox inside the webOS 4 emulator, as
# root, the way the TV app starts (through the geckotv launcher). Copied into
# the app folder by hand; see build/emu/README.md.
APP=$(cd "$(dirname "$0")" && pwd)
# The executables use the bundled glibc's loader through this link (build/emu/
# set-interp.py); the bundled glibc sits next to Firefox's own libraries.
ln -sf "$APP/firefox-runtime/ld-linux.so.2" /tmp/ld32.so
cd "$APP" || exit 1
echo "----- $(date) -----" >> "$APP/geckotv.log"
# The library path goes only on the launcher itself: a system program given
# it would load the bundled glibc under the system's older loader.
setsid sh -c 'LD_LIBRARY_PATH="$0/firefox-runtime" exec "$0/geckotv"' "$APP" \
    < /dev/null >> "$APP/geckotv.log" 2>&1 &
echo "started"
