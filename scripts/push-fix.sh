#!/bin/sh
# Copy the rebuilt adapter library and launcher to the installed app on the
# TV without reinstalling the IPK. Keeps timestamped backups next to the
# originals. Usage: ./scripts/push-fix.sh   (TV_IP and SSH_KEY as in
# install2tvfrommacos.sh)
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
TV_IP=${TV_IP:-192.168.0.79}
SSH_KEY=${SSH_KEY:-$HOME/.ssh/webos_deploy}
APP=/media/developer/apps/usr/palm/applications/com.github.gprot42.geckotv
TS=$(date +%Y%m%d-%H%M%S)
SSH="ssh -o BatchMode=yes -o ConnectTimeout=8 -o StrictHostKeyChecking=no -i $SSH_KEY root@$TV_IP"
SCP="scp -o BatchMode=yes -o StrictHostKeyChecking=no -i $SSH_KEY"

for f in app/firefox-runtime/libwayland-client.so.0 app/geckotv app/geckotv.sh \
         app/appinfo.json app/icon.png app/largeIcon.png; do
    [ -f "$ROOT/$f" ] || { echo "missing $ROOT/$f, run make and build/build-adapter.sh" >&2; exit 1; }
done

# Count real Firefox processes. busybox ps truncates the command line, and a
# shell whose own arguments contain the pattern matches itself, so test argv[0]
# only and skip our own pid.
running=$($SSH 'n=0; self=$$; for p in /proc/[0-9]*; do
    pid=${p#/proc/}; [ "$pid" = "$self" ] && continue
    [ -r "$p/cmdline" ] || continue
    a0=$(tr "\0" "\n" < "$p/cmdline" 2>/dev/null | head -1)
    case "$a0" in */firefox-runtime/firefox) n=$((n+1));; esac
done; echo $n')
if [ "${running:-0}" -gt 0 ]; then
    echo "Firefox is running on the TV ($running processes). Close it first." >&2
    exit 1
fi
echo "Firefox not running ($running processes), proceeding."

$SSH "cd $APP && cp firefox-runtime/libwayland-client.so.0 firefox-runtime/libwayland-client.so.0.bak-$TS \
    && cp geckotv geckotv.bak-$TS && cp geckotv.sh geckotv.sh.bak-$TS"
$SCP "$ROOT/app/firefox-runtime/libwayland-client.so.0" "root@$TV_IP:$APP/firefox-runtime/libwayland-client.so.0.new"
$SCP "$ROOT/app/geckotv" "root@$TV_IP:$APP/geckotv.new"
$SCP "$ROOT/app/geckotv.sh" "root@$TV_IP:$APP/geckotv.sh.new"
# Manifest and icons: the launcher reads these, so they must land too.
$SCP "$ROOT/app/appinfo.json" "root@$TV_IP:$APP/appinfo.json.new"
$SCP "$ROOT/app/icon.png" "root@$TV_IP:$APP/icon.png.new"
$SCP "$ROOT/app/largeIcon.png" "root@$TV_IP:$APP/largeIcon.png.new"
# Minimal FFmpeg (H.264/AAC/MP3 decoders only), built by build/ffmpeg-mini.sh.
for f in libavcodec.so.60 libavutil.so.58 libswresample.so.4; do
    [ -f "$ROOT/app/firefox-runtime/$f" ] && $SCP "$ROOT/app/firefox-runtime/$f" "root@$TV_IP:$APP/firefox-runtime/$f"
done
$SCP "$ROOT/app/defaults/pref/00-webos.js" \
    "root@$TV_IP:$APP/firefox-runtime/defaults/pref/00-webos.js"
$SSH "cd $APP && cp appinfo.json appinfo.json.bak-$TS \
    && mv firefox-runtime/libwayland-client.so.0.new firefox-runtime/libwayland-client.so.0 \
    && mv geckotv.new geckotv && mv geckotv.sh.new geckotv.sh \
    && mv appinfo.json.new appinfo.json && mv icon.png.new icon.png && mv largeIcon.png.new largeIcon.png \
    && chmod 755 firefox-runtime/libwayland-client.so.0 geckotv geckotv.sh \
    && chmod 644 appinfo.json icon.png largeIcon.png \
    && md5sum firefox-runtime/libwayland-client.so.0 geckotv icon.png"
echo "local:"
md5 -q "$ROOT/app/firefox-runtime/libwayland-client.so.0" 2>/dev/null || md5sum "$ROOT/app/firefox-runtime/libwayland-client.so.0"
md5 -q "$ROOT/app/geckotv" 2>/dev/null || md5sum "$ROOT/app/geckotv"
echo "Backups on the TV carry the suffix .bak-$TS. To roll back:"
echo "  ssh -i $SSH_KEY root@$TV_IP 'cd $APP && cp firefox-runtime/libwayland-client.so.0.bak-$TS firefox-runtime/libwayland-client.so.0 && cp geckotv.bak-$TS geckotv'"
