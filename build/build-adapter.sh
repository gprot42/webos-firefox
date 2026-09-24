#!/bin/bash
# Rebuild libwayland-client with the webOS shell adapter baked in, for the
# native 32-bit Firefox. Run inside ffbuild:
#   podman exec ffbuild bash /src/build/build-adapter.sh
set -euo pipefail
ROOT=/src
WL=/tmp/wayland-1.22.0
GEN=$WL/src
if [[ ! -f $WL/src/wayland-client.c ]]; then
    curl -fsSL -o /tmp/wayland-1.22.0.tar.xz \
        https://gitlab.freedesktop.org/wayland/wayland/-/releases/1.22.0/downloads/wayland-1.22.0.tar.xz
    tar -C /tmp -xf /tmp/wayland-1.22.0.tar.xz
fi
wayland-scanner client-header /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml \
    "$GEN/xdg-shell-client-protocol.h"
wayland-scanner client-header "$ROOT/src/webos-shell.xml" \
    "$GEN/wayland-webos-shell-client-protocol.h"
wayland-scanner private-code "$ROOT/src/webos-shell.xml" "$GEN/webos-shell-protocol.c"
wayland-scanner client-header "$ROOT/src/webos-input-manager.xml" \
    "$GEN/webos-input-manager-client-protocol.h"
wayland-scanner private-code "$ROOT/src/webos-input-manager.xml" \
    "$GEN/webos-input-manager-protocol.c"
wayland-scanner client-header "$ROOT/src/text-model.xml" \
    "$GEN/text-model-client-protocol.h"
wayland-scanner private-code "$ROOT/src/text-model.xml" \
    "$GEN/text-model-protocol.c"
cp "$ROOT/src/webos-xdg.c" "$GEN/webos-xdg.c"
python3 - <<'PY'
from pathlib import Path
p = Path("/tmp/wayland-1.22.0/src/wayland-client.c")
text = p.read_text()
if "webos_xdg_intercept(" not in text:
    key = "uint32_t flags, union wl_argument *args)\n{\n"
    at = text.find(key)
    if at < 0:
        raise SystemExit("marshal site not found")
    insert = (
        "uint32_t flags, union wl_argument *args)\n{\n"
        "\tstruct wl_proxy *webos_ret = NULL;\n"
        "\tif (webos_xdg_intercept(proxy, opcode, interface, version, flags, args, &webos_ret))\n"
        "\t\treturn webos_ret;\n\n"
    )
    text = text[:at] + insert + text[at + len(key):]
if "webos_xdg_prepare_listener(" not in text:
    key = "\tproxy->object.implementation = implementation;\n"
    at = text.find(key)
    if at < 0:
        raise SystemExit("listener site not found")
    text = text[:at] + "\twebos_xdg_prepare_listener(proxy, &implementation, &data);\n" + text[at:]
decl = (
    "extern int webos_xdg_intercept(struct wl_proxy *proxy, uint32_t opcode,\n"
    "\t\tconst struct wl_interface *interface, uint32_t version,\n"
    "\t\tuint32_t flags, union wl_argument *args, struct wl_proxy **out);\n"
    "extern void webos_xdg_prepare_listener(struct wl_proxy *proxy,\n"
    "\t\tvoid (***implementation)(void), void **data);\n\n"
)
if "extern int webos_xdg_intercept" not in text:
    marker = '#include "wayland-private.h"\n'
    at = text.find(marker)
    if at < 0:
        raise SystemExit("private header include not found")
    at += len(marker)
    text = text[:at] + "\n" + decl + text[at:]
ready_decl = "extern void webos_xdg_listener_ready(struct wl_proxy *proxy);\n"
if "webos_xdg_listener_ready" not in text:
    needle = "extern void webos_xdg_prepare_listener"
    at = text.find(needle)
    if at < 0:
        raise SystemExit("listener decl not found")
    line_end = text.find("\n", at)
    # declaration spans two lines
    line_end = text.find("\n", line_end + 1)
    text = text[: line_end + 1] + ready_decl + text[line_end + 1 :]
if "webos_xdg_listener_ready(proxy)" not in text:
    key = "webos_xdg_prepare_listener(proxy, &implementation, &data);\n"
    at = text.find(key)
    if at < 0:
        raise SystemExit("listener call not found")
    # Insert after user_data is published so configure handlers can read it.
    user = "proxy->user_data = data;\n"
    user_at = text.find(user, at)
    if user_at < 0:
        raise SystemExit("user_data assign not found")
    insert_at = user_at + len(user)
    text = text[:insert_at] + "\twebos_xdg_listener_ready(proxy);\n" + text[insert_at:]
if "webos_xdg_create_virtual" not in text:
    fn = (
        "\nstruct wl_proxy *\n"
        "webos_xdg_create_virtual(struct wl_proxy *factory,\n"
        "\t\tconst struct wl_interface *interface, uint32_t version)\n"
        "{\n"
        "\tstruct wl_proxy *proxy;\n"
        "\tstruct wl_display *display = factory->display;\n"
        "\n"
        "\t/* Do not take a client id. The server rejects the next real\n"
        "\t * id when a number was allocated here and never sent.\n"
        "\t */\n"
        "\tpthread_mutex_lock(&display->mutex);\n"
        "\tproxy = zalloc(sizeof *proxy);\n"
        "\tif (proxy) {\n"
        "\t\tproxy->object.interface = interface;\n"
        "\t\tproxy->object.id = 0;\n"
        "\t\tproxy->display = display;\n"
        "\t\tproxy->queue = factory->queue;\n"
        "\t\tproxy->refcount = 1;\n"
        "\t\tproxy->version = version ? version : 1;\n"
        "\t\twl_list_init(&proxy->queue_link);\n"
        "\t}\n"
        "\tpthread_mutex_unlock(&display->mutex);\n"
        "\treturn proxy;\n"
        "}\n"
        "\n"
        "void\n"
        "webos_xdg_destroy_virtual(struct wl_proxy *proxy)\n"
        "{\n"
        "\tfree(proxy);\n"
        "}\n\n"
    )
    key = "WL_EXPORT struct wl_proxy *\nwl_proxy_marshal_array_flags("
    at = text.find(key)
    if at < 0:
        raise SystemExit("virtual ctor site not found")
    text = text[:at] + fn + text[at:]
if "webos_xdg_wrapper_destroy_report" not in text:
    # Recover from wl_proxy_destroy() on a wrapper instead of aborting, and
    # report the caller. Both paths into this check hold the display mutex,
    # so do wl_proxy_wrapper_destroy()'s cleanup inline.
    old = ('\tif (proxy->flags & WL_PROXY_FLAG_WRAPPER)\n'
           '\t\twl_abort("Tried to destroy wrapper with wl_proxy_destroy()\\n");\n')
    new = ('\tif (proxy->flags & WL_PROXY_FLAG_WRAPPER) {\n'
           '\t\twebos_xdg_wrapper_destroy_report(proxy, webos_outer_caller);\n'
           '\t\twl_list_remove(&proxy->queue_link);\n'
           '\t\tfree(proxy);\n'
           '\t\treturn;\n'
           '\t}\n')
    if old not in text:
        raise SystemExit("wrapper abort site not found")
    text = text.replace(old, new, 1)
    decl = ("extern void webos_xdg_wrapper_destroy_report(struct wl_proxy *proxy, void *caller);\n"
            "static __thread void *webos_outer_caller;\n")
    marker = '#include "wayland-private.h"\n'
    text = text.replace(marker, marker + decl, 1)
    for sig in ("WL_EXPORT void\nwl_proxy_destroy(struct wl_proxy *proxy)\n{\n",):
        if sig not in text:
            raise SystemExit("wl_proxy_destroy not found")
        text = text.replace(sig, sig + "\twebos_outer_caller = __builtin_return_address(0);\n", 1)
    i = text.index("wl_proxy_marshal_flags(struct wl_proxy *proxy")
    j = text.index("{\n", i) + 2
    text = text[:j] + "\twebos_outer_caller = __builtin_return_address(0);\n" + text[j:]
if "webos_xdg_last_caller" not in text:
    # Let the adapter name the library that added a listener.
    sig = ("wl_proxy_add_listener(struct wl_proxy *proxy,\n"
           "\t\t      void (**implementation)(void), void *data)\n{\n")
    if sig not in text:
        raise SystemExit("wl_proxy_add_listener not found")
    text = text.replace(sig, sig + "\twebos_outer_caller = __builtin_return_address(0);\n", 1)
    decl = "static __thread void *webos_outer_caller;\n"
    text = text.replace(decl, decl + "void *webos_xdg_last_caller(void);\n", 1)
    text += "\nvoid *\nwebos_xdg_last_caller(void)\n{\n\treturn webos_outer_caller;\n}\n"
if "webos_xdg_virtual_destroyed" not in text:
    # Virtual proxies have id 0. Older generated code (wayland-scanner
    # before 1.20, as in Debian 11's GTK) calls wl_proxy_destroy() after the
    # destroy request, so hand those to the adapter instead of the id map.
    sig = "WL_EXPORT void\nwl_proxy_destroy(struct wl_proxy *proxy)\n{\n"
    at = text.find(sig)
    if at < 0:
        raise SystemExit("wl_proxy_destroy not found")
    at = text.index("\n", text.index("webos_outer_caller = ", at)) + 1
    text = (text[:at] +
            "\tif (proxy->object.id == 0 && !(proxy->flags & WL_PROXY_FLAG_WRAPPER)) {\n"
            "\t\twebos_xdg_virtual_destroyed(proxy);\n"
            "\t\treturn;\n"
            "\t}\n" + text[at:])
    decl = "static __thread void *webos_outer_caller;\n"
    text = text.replace(decl, decl + "void webos_xdg_virtual_destroyed(struct wl_proxy *proxy);\n", 1)
if "webos_xdg_proxy_display" not in text:
    # Let the adapter tell GDK's own display and registry apart from the
    # wrappers and private queues that libraries such as Mali's EGL create.
    decl = "static __thread void *webos_outer_caller;\n"
    text = text.replace(decl, decl +
        "struct wl_display *webos_xdg_proxy_display(struct wl_proxy *proxy);\n"
        "int webos_xdg_on_default_queue(struct wl_proxy *proxy);\n", 1)
    text += ("\nstruct wl_display *\nwebos_xdg_proxy_display(struct wl_proxy *proxy)\n{\n"
             "\treturn proxy->display;\n}\n"
             "\nint\nwebos_xdg_on_default_queue(struct wl_proxy *proxy)\n{\n"
             "\treturn proxy->queue == &proxy->display->default_queue;\n}\n")
if "webos_xdg_null_queue_report" not in text:
    # A proxy whose queue was destroyed has queue == NULL; wrapping it
    # would crash. Report it and fall back to the default queue.
    old = "\twrapper->queue = wrapped_proxy->queue;\n"
    if old not in text:
        raise SystemExit("wrapper queue copy not found")
    text = text.replace(old,
        "\twrapper->queue = wrapped_proxy->queue;\n"
        "\tif (!wrapper->queue) {\n"
        "\t\twebos_xdg_null_queue_report(wrapped_proxy, __builtin_return_address(0));\n"
        "\t\twrapper->queue = &wrapped_proxy->display->default_queue;\n"
        "\t}\n", 1)
    decl = "static __thread void *webos_outer_caller;\n"
    text = text.replace(decl, decl +
        "void webos_xdg_null_queue_report(struct wl_proxy *proxy, void *caller);\n", 1)
p.write_text(text)
mb = Path("/tmp/wayland-1.22.0/src/meson.build")
m = mb.read_text()
old = "'wayland-client.c'\n"
new = "'wayland-client.c',\n\t'webos-xdg.c',\n\t'webos-shell-protocol.c'\n"
if "webos-xdg.c" not in m:
    if old not in m:
        raise SystemExit("meson source not found")
    m = m.replace(old, new, 1)
if "webos-input-manager-protocol.c" not in m:
    needle = "'webos-shell-protocol.c'"
    if needle not in m:
        raise SystemExit("shell protocol source missing")
    m = m.replace(needle, "'webos-shell-protocol.c',\n\t'webos-input-manager-protocol.c'", 1)
if "text-model-protocol.c" not in m:
    needle = "'webos-input-manager-protocol.c'"
    if needle not in m:
        raise SystemExit("input manager source missing")
    m = m.replace(needle, "'webos-input-manager-protocol.c',\n\t'text-model-protocol.c'", 1)
mb.write_text(m)
print("patched")
PY
cd "$WL"
# 32-bit ARMv7 softfp. Default: the Debian armel sysroot (build/arm32-cross.ini).
# CROSS=nc4 builds against the buildroot-nc4 SDK instead (build/nc4/cross.ini).
case "${CROSS:-arm32}" in
    nc4) CROSS_FILE=$ROOT/build/nc4/cross.ini; BUILD=build-nc4 ;;
    *) CROSS_FILE=$ROOT/build/arm32-cross.ini; BUILD=build-arm32 ;;
esac
if [[ ! -f $BUILD/build.ninja ]]; then
    meson setup "$BUILD" --cross-file "$CROSS_FILE" \
        -Ddocumentation=false -Ddtd_validation=false -Dtests=false -Dscanner=false
fi
ninja -C "$BUILD" src/libwayland-client.so.0.22.0
OUT=${OUT:-$ROOT/app/firefox-runtime}
mkdir -p "$OUT"
install -m 0755 "$BUILD/src/libwayland-client.so.0.22.0" "$OUT/libwayland-client.so.0"
echo ADAPTER_OK
file "$OUT/libwayland-client.so.0"
