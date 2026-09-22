#!/bin/sh
# Native entry point. webOS passes a JSON launch blob as $1. Drop it.
APP_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd) || exit 1

mkdir -p "$APP_DIR/profile/cache" "$APP_DIR/profile/config" /tmp/xdg

export HOME="$APP_DIR/profile"
export XDG_CONFIG_HOME="$APP_DIR/profile/config"
export XDG_CACHE_HOME="$APP_DIR/profile/cache"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/tmp/xdg}"
export XKB_CONFIG_ROOT="${XKB_CONFIG_ROOT:-/usr/share/X11/xkb}"
export GDK_BACKEND=wayland
export MOZ_ENABLE_WAYLAND=1
export MOZ_DBUS_REMOTE=0
export MOZ_DISABLE_CONTENT_SANDBOX=1
export MOZ_DISABLE_GMP_SANDBOX=1
export MOZ_DISABLE_RDD_SANDBOX=1
export MOZ_DISABLE_SOCKET_PROCESS_SANDBOX=1
export MOZ_DISABLE_GPU_SANDBOX=1

BRIDGE_LIB=/media/developer/apps/usr/palm/applications/org.webosbrew.bridge-64to32/lib
LIBPATH="$BRIDGE_LIB"
if [ -d "$APP_DIR/lib" ]; then
    LIBPATH="$APP_DIR/lib:$LIBPATH"
fi
if [ -d "$APP_DIR/firefox-runtime" ]; then
    LIBPATH="$APP_DIR/firefox-runtime:$LIBPATH"
fi
export LD_LIBRARY_PATH="$LIBPATH${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

cd "$APP_DIR" || exit 1
exec >>"$APP_DIR/geckotv.log" 2>&1
echo "----- $(date) -----"

if [ "$#" -gt 0 ]; then
    case "$1" in
        '{'*) shift ;;
    esac
fi

if [ -e "$APP_DIR/wayland-debug" ]; then
    export WAYLAND_DEBUG=1
    echo "WAYLAND_DEBUG=1 because $APP_DIR/wayland-debug exists"
fi

if [ -x "$APP_DIR/firefox-runtime/firefox" ]; then
    exec "$APP_DIR/firefox-runtime/firefox" --profile "$HOME" --no-remote "$@"
fi

if [ -x "$APP_DIR/firefox" ]; then
    exec "$APP_DIR/firefox" --profile "$HOME" --no-remote "$@"
fi

exec "$APP_DIR/smoke" "$@"
