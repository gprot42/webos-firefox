#!/bin/sh
# Collect what is needed to debug the browser on a TV into one file, so a
# problem can be reported in one go. Run as root on the TV:
#   sh /media/developer/apps/usr/palm/applications/com.github.gprot42.geckotv/diagnose.sh
# It restarts the browser once with extra logging and a test pattern (a grey
# screen with a moving white bar, drawn by the window itself), then writes
# /tmp/geckotv-report.txt. It takes about two minutes. Send that file, and
# say what the TV showed.
A=$(cd "$(dirname "$0")" && pwd)
ID=com.github.gprot42.geckotv
R=/tmp/geckotv-report.txt
START=/tmp/geckotv-diagnose.start

say() { echo "$*"; echo "$*" >> "$R"; }
section() { echo "" >> "$R"; echo "== $*" >> "$R"; }

: > "$R"
section "system"
{
    date
    grep '"version"' "$A/appinfo.json"
    grep -hE '"(webos_release|core_os_release|device_name|webos_name)"' /var/run/nyx/os_info.json 2>/dev/null
    uname -a
    echo "libc: $(/lib/libc.so.6 2>/dev/null | head -n 1)"
    id
    ls -la "$A" | head -n 20
    echo "-- env file:"; cat "$A/env" 2>/dev/null
} >> "$R" 2>&1

say "Stopping the browser."
kill $(pidof firefox) 2>/dev/null
sleep 3
kill -9 $(pidof firefox) 2>/dev/null

# Extra settings for this run only; the env file is restored afterwards.
if [ -f "$A/env" ]; then cp "$A/env" /tmp/geckotv-env.saved; else rm -f /tmp/geckotv-env.saved; fi
{
    [ -f /tmp/geckotv-env.saved ] && grep -v '^WEBOS_XDG_\(OV_DEBUG\|FLAT_TEST\)=' /tmp/geckotv-env.saved
    echo WEBOS_XDG_OV_DEBUG=1
    echo WEBOS_XDG_FLAT_TEST=1
} > "$A/env.new" && mv "$A/env.new" "$A/env"
touch "$A/wayland-debug" "$START"

say "Starting the browser. Please watch the TV for the next 40 seconds (the whole script takes about two minutes)."
luna-send -n 1 luna://com.webos.applicationManager/launch "{\"id\":\"$ID\"}" >> "$R" 2>&1
sleep 40
rm -f "$A/wayland-debug"

LOG=$(find "$A" /tmp /home /var /media -name geckotv.log -newer "$START" 2>/dev/null | head -n 1)
section "log: ${LOG:-none found}"
L=""
if [ -n "$LOG" ]; then
    # The log grows across runs: keep this run, from the launcher's last start.
    L=/tmp/geckotv-run.log
    n=$(awk '/^geckotv: profile/ { n = NR } END { print n + 0 }' "$LOG")
    [ "$n" -gt 0 ] || n=1
    tail -n +"$n" "$LOG" > "$L"
    grep -m3 '^geckotv' "$L" >> "$R"
    {
        echo "flat window commits: $(grep -c 'overlay commit' "$L")"
        echo "buffers released (flat window): $(grep -c 'overlay buffer .* released' "$L")"
        echo "reusing buffers: $(grep -c 'keeps its buffers' "$L")"
        echo "firefox draws: $(grep -c ' draws ' "$L")"
        echo "all wl_buffer releases: $(grep -cE 'wl_buffer@[0-9]+\.release' "$L")"
        echo "all frame callbacks answered: $(grep -cE 'wl_callback@[0-9]+\.done' "$L")"
        echo "adapter's own answers: $(grep 'flat: ' "$L" | grep -v 'cannot copy' | tail -n 1 | sed 's/.*flat: //')"
    } >> "$R"
    # The flat window is the first surface given a wl_shell role.
    W=$(grep -m1 -oE 'wl_shell@[0-9]+\.get_shell_surface\(new id wl_shell_surface@[0-9]+, wl_surface@[0-9]+' "$L" |
        sed 's/.*wl_surface@//')
    if [ -n "$W" ]; then
        awk -v w="wl_surface@$W" '
            index($0, w ".frame(new id wl_callback@") {
                s = $0; sub(/.*new id wl_callback@/, "", s); sub(/\).*/, "", s); cb[s] = 1; asked++ }
            /wl_callback@[0-9]+\.done/ {
                s = $0; sub(/.*wl_callback@/, "", s); sub(/\..*/, "", s); if (s in cb) answered++ }
            index($0, w ".commit") { commits++ }
            index($0, w ".enter") { enters++ }
            END { printf "window %s: commits %d, frame callbacks asked %d answered %d, output enter %d\n",
                         w, commits, asked, answered, enters }' "$L" >> "$R"
    fi
    section "adapter"
    grep -E 'webos-xdg: |signal |Protocol error' "$L" |
        grep -vE 'listener wl_|virt kind|remote /dev|compositor offers|seat added|inject |global wl_|bound virtual|clamp compositor|KEYBOARD|POINTER|translate|Wayland protocol error|overlay commit buffer|overlay buffer .* released' |
        cut -c1-200 | head -n 40 >> "$R"
    section "compositor globals"
    grep -o 'compositor offers .*' "$L" | sort -u | sed 's/compositor offers //' | head -n 40 | tr '\n' ',' | sed 's/,$/\n/' >> "$R"
    section "surfaces: buffers attached (per surface), and where buffers came from"
    grep -oE 'wl_surface@[0-9]+\.attach\(wl_buffer@[0-9]+' "$L" | sed 's/(wl_buffer@.*//' | sort | uniq -c >> "$R"
    echo "shm buffers made: $(grep -cE 'wl_shm_pool@[0-9]+\.create_buffer' "$L"), mali buffers made: $(grep -ciE 'mali_buffer_sharing@[0-9]+\.create' "$L")" >> "$R"
    section "graphics"
    grep -E 'GFX|EGL|GLX' "$L" | sed 's/^ *(t=[0-9.]*) *//; s/^Crash Annotation GraphicsCriticalError: //' | sort -u | head -n 8 >> "$R"
    # A crash or an exit shows at the end of the log.
    section "end of the log"
    grep -vE 'listener wl_|virt kind|^\[' "$L" | tail -n 30 | cut -c1-200 >> "$R"
fi

section "firefox threads"
P=""
for p in $(pidof firefox); do
    tr '\0' ' ' < /proc/$p/cmdline | grep -q -- --profile && P=$p
done
echo "firefox processes: $(pidof firefox | wc -w), main $P" >> "$R"
[ -n "$P" ] && for t in /proc/$P/task/*; do echo "$(cat $t/comm) $(cat $t/wchan)"; done |
    sort | uniq -c | sort -rn | head -n 25 >> "$R"

section "system log"
grep -E "$ID|NL_VSC" /var/log/messages 2>/dev/null | tail -n 12 | cut -c1-220 >> "$R"
dmesg | grep -iE 'oom|killed process|segfault' | tail -n 5 >> "$R"

# Restore the settings and stop the test run.
kill $(pidof firefox) 2>/dev/null
sleep 3
if [ -f /tmp/geckotv-env.saved ]; then mv /tmp/geckotv-env.saved "$A/env"; else rm -f "$A/env"; fi

section "gpuprobe"
if [ -x "$A/gpuprobe" ]; then
    say "Running the GPU probe (up to a minute; the TV may flash)."
    "$A/gpuprobe" 2>&1 | tail -n 30 | cut -c1-200 >> "$R"
fi
rm -f "$START" /tmp/geckotv-run.log

echo ""
echo "Done. The report is $R ($(wc -l < "$R") lines)."
echo "Please send that file, and say what the TV showed during the 40 seconds:"
echo "  a) black"
echo "  b) grey with a white bar moving along the bottom"
echo "  c) grey with a white bar that did not move"
echo "  d) the browser (with or without the bar)"
echo "  e) something else"
