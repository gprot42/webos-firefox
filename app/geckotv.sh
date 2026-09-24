#!/bin/sh
# Alternative entry point. appinfo.json's "main" is the geckotv program, which
# sets up everything Firefox needs on each webOS version (library path and the
# libwayland-egl fallback, GTK input-method, schemas, keymap data, launch URL).
# Hand over to it so both entry points behave the same; a script copy of that
# logic went stale once and failed on webOS 4 (no libwayland-egl.so.1).
APP_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd) || exit 1

if [ -x "$APP_DIR/geckotv" ]; then
    exec "$APP_DIR/geckotv" "$@"
fi

# No launcher in this package (smoke builds only): run the GLES test client.
cd "$APP_DIR" || exit 1
exec >>"$APP_DIR/geckotv.log" 2>&1
echo "----- $(date) ----- no geckotv launcher, running smoke"
exec "$APP_DIR/smoke"
