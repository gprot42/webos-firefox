#!/bin/bash
# Drive the local QEMU copy of LG's webOS TV 4.0 or 6.0 emulator (WEBOS=6
# for 6.0), used to test Firefox on those compositors. One entry point, so a single permission
# rule covers every emulator action.
#   emu.sh start | stop | status
#   emu.sh shot [file.png]         screenshot of the emulator's screen
#   emu.sh click X Y               click at screen pixel X,Y (1920x1080)
#   emu.sh key KEY...              press keys (QEMU names: ret, tab, a, shift-a)
#   emu.sh ssh 'command'           run a command as root in the emulator
#   emu.sh put <local> <remote>    copy a file into the emulator
#   emu.sh get <remote> <local>    copy a file out of the emulator
#   emu.sh deploy                  copy the x86 test build of Firefox in
#   emu.sh firefox                 (re)start Firefox in the emulator
#   emu.sh install                 install the x86 IPK through webOS's installer
#   emu.sh launch                  start the installed app as a TV does
# EMU_DIR holds the disk image (webos4.raw) and root_key, as set up by
# build/emu/setup-emulator.sh; see build/emu/README.md.
set -euo pipefail
WEBOS=${WEBOS:-4}
EMU_DIR=${EMU_DIR:-$HOME/webos$WEBOS-emulator}
APP=/media/developer/apps/usr/palm/applications/com.github.gprot42.geckotv
KEY=$EMU_DIR/root_key
# The emulator runs dropbear 2016, which only knows ssh-rsa signatures.
SSH_OPTS=(-i "$KEY" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null
          -o BatchMode=yes -o ConnectTimeout=10 -o LogLevel=ERROR
          -o HostKeyAlgorithms=+ssh-rsa -o PubkeyAcceptedAlgorithms=+ssh-rsa)
MON_PORT=4444
QMP_PORT=4445

monitor() { printf '%s\n' "$1" | nc -w 2 127.0.0.1 "$MON_PORT" >/dev/null 2>&1; }
# QMP, for absolute pointer events (the monitor's mouse_move is relative).
qmp_input() {
    { echo '{"execute":"qmp_capabilities"}'; sleep 0.2
      for ev in "$@"; do
          echo "{\"execute\":\"input-send-event\",\"arguments\":{\"events\":[$ev]}}"
          sleep 0.15
      done; } | nc -w 2 127.0.0.1 "$QMP_PORT" >/dev/null 2>&1
}
running() { [ -f "$EMU_DIR/qemu.pid" ] && kill -0 "$(cat "$EMU_DIR/qemu.pid")" 2>/dev/null; }

case "${1:-}" in
start)
    running && { echo "already running"; exit 0; }
    # Settings follow LG_webOS_TV_Emulator.vbox: PIIX IDE disk, a NIC with
    # the SDK's port forwards (host 127.0.0.1 only; PCnet in 4.0, e1000 in
    # 6.0), USB tablet, AC97, and a display: a plain VGA framebuffer for 4.0;
    # 6.0's compositor needs a DRM device, which virtio-vga provides.
    nic=pcnet vga="VGA,vgamem_mb=32"
    [ "$WEBOS" = 6 ] && nic=e1000 vga=virtio-vga,xres=1920,yres=1080
    qemu-system-i386 -name webos$WEBOS \
      -machine pc -accel tcg,thread=multi,tb-size=1024 -cpu max -smp 3 -m 2048 \
      -drive file="$EMU_DIR/webos$WEBOS.raw",format=raw,if=ide,cache=writeback \
      -netdev user,id=n0,hostfwd=tcp:127.0.0.1:6622-:22,hostfwd=tcp:127.0.0.1:6623-:2222,hostfwd=tcp:127.0.0.1:9998-:9998,hostfwd=tcp:127.0.0.1:19001-:19001 \
      -device $nic,netdev=n0,mac=08:00:27:a2:0d:39 \
      -device pci-ohci,id=ohci -device usb-tablet,bus=ohci.0 \
      -audiodev none,id=snd0 -device AC97,audiodev=snd0 \
      -device $vga \
      -display none -monitor tcp:127.0.0.1:$MON_PORT,server,nowait \
      -qmp tcp:127.0.0.1:$QMP_PORT,server,nowait \
      -serial file:"$EMU_DIR/serial.log" -rtc base=utc \
      -daemonize -pidfile "$EMU_DIR/qemu.pid"
    echo "started, pid $(cat "$EMU_DIR/qemu.pid")" ;;
stop)
    running || { echo "not running"; exit 0; }
    # The guest ignores ACPI power-off; ext3's journal recovers on next boot.
    ssh "${SSH_OPTS[@]}" -p 6623 root@127.0.0.1 'sync' 2>/dev/null || true
    monitor quit
    echo stopped ;;
status)
    running && echo "running, pid $(cat "$EMU_DIR/qemu.pid")" || echo "not running" ;;
shot)
    out=${2:-$EMU_DIR/shot.png}
    rm -f "$out"
    monitor "screendump $out -f png"
    for _ in $(seq 1 25); do [ -s "$out" ] && break; sleep 0.2; done
    [ -s "$out" ] && echo "$out" || { echo "no screenshot"; exit 1; } ;;
click)
    # The USB tablet takes absolute positions scaled to 0..32767.
    x=$(( $2 * 32767 / 1920 )); y=$(( $3 * 32767 / 1080 ))
    # Approach from a few pixels away and let webOS catch up (its first move
    # after a while can land on one axis only), so the target gets a real
    # motion event (webOS 4 gives the entry event screen-scale coordinates).
    near="{\"type\":\"abs\",\"data\":{\"axis\":\"x\",\"value\":$((x - 200))}},{\"type\":\"abs\",\"data\":{\"axis\":\"y\",\"value\":$((y - 200))}}"
    move="{\"type\":\"abs\",\"data\":{\"axis\":\"x\",\"value\":$x}},{\"type\":\"abs\",\"data\":{\"axis\":\"y\",\"value\":$y}}"
    qmp_input "$near"
    sleep 0.5
    qmp_input "$move"
    sleep 0.5
    qmp_input "$move" '{"type":"btn","data":{"down":true,"button":"left"}}' \
              '{"type":"btn","data":{"down":false,"button":"left"}}'
    echo "clicked $2,$3" ;;
key)
    shift
    for k in "$@"; do monitor "sendkey $k"; sleep 0.2; done ;;
ssh)
    shift
    exec ssh "${SSH_OPTS[@]}" -p 6623 root@127.0.0.1 "$@" ;;
put)
    exec scp -O "${SSH_OPTS[@]}" -P 6623 "$2" "root@127.0.0.1:$3" ;;
get)
    exec scp -O "${SSH_OPTS[@]}" -P 6623 "root@127.0.0.1:$2" "$3" ;;
deploy)
    # Built by build/emu/assemble-emu.sh inside ffbuild.
    podman cp ffbuild:/work/emu-app.tar.gz "$EMU_DIR/emu-app.tar.gz"
    scp -O "${SSH_OPTS[@]}" -P 6623 "$EMU_DIR/emu-app.tar.gz" root@127.0.0.1:/media/developer/emu-app.tar.gz
    scp -O "${SSH_OPTS[@]}" -P 6623 "$(dirname "$0")/run-firefox.sh" root@127.0.0.1:/media/developer/run-firefox.sh
    ssh "${SSH_OPTS[@]}" -p 6623 root@127.0.0.1 "set -e
        P=\$(pidof firefox firefox-bin || true); [ -z \"\$P\" ] || kill \$P
        mkdir -p $(dirname "$APP"); cd $(dirname "$APP")
        rm -rf emu-app $APP; tar -xzf /media/developer/emu-app.tar.gz; mv emu-app $APP
        mv /media/developer/run-firefox.sh $APP/; chmod +x $APP/run-firefox.sh
        rm /media/developer/emu-app.tar.gz; du -sh $APP"
    rm "$EMU_DIR/emu-app.tar.gz" ;;
firefox)
    exec ssh "${SSH_OPTS[@]}" -p 6623 root@127.0.0.1 \
        "P=\$(pidof firefox firefox-bin); [ -z \"\$P\" ] || { kill \$P; sleep 2; }; $APP/run-firefox.sh" ;;
install)
    # dist/*_i586.ipk from assemble-emu.sh, through webOS's own installer (the
    # way Developer Mode and Homebrew Channel install on a TV: same owner,
    # same folder), replacing a copy from `deploy`. Then the emulator-only
    # extras a package cannot carry: the env file (MIME database; on 6.0 the
    # TV's compositor path) and the /tmp/ld32.so loader link.
    ipk=$(ls -t "$(dirname "$0")"/../../dist/*_i586.ipk 2>/dev/null | head -1)
    [ -n "$ipk" ] || { echo "no dist/*_i586.ipk: run assemble-emu.sh"; exit 1; }
    scp -O "${SSH_OPTS[@]}" -P 6623 "$ipk" root@127.0.0.1:/media/developer/emu-install.ipk
    if [ "$WEBOS" = 6 ]; then
        # sam starts native apps through /usr/bin/jailer, which 6.0 lacks.
        podman cp ffbuild:/work/emu-jailer "$EMU_DIR/emu-jailer"
        scp -O "${SSH_OPTS[@]}" -P 6623 "$EMU_DIR/emu-jailer" root@127.0.0.1:/usr/bin/jailer
        rm "$EMU_DIR/emu-jailer"
    fi
    ssh "${SSH_OPTS[@]}" -p 6623 root@127.0.0.1 "
        l() { luna-send -n \$1 -f \$2 \"\$3\" > /tmp/ls.out 2>&1 & p=\$!; sleep \$4; kill \$p 2>/dev/null; cat /tmp/ls.out; }
        P=\$(pidof firefox); [ -z \"\$P\" ] || kill \$P
        l 1 luna://com.webos.appInstallService/dev/remove '{\"id\":\"com.github.gprot42.geckotv\"}' 20 >/dev/null
        rm -rf $APP
        l 30 luna://com.webos.appInstallService/dev/install '{\"id\":\"com.github.gprot42.geckotv\",\"ipkUrl\":\"/media/developer/emu-install.ipk\",\"subscribe\":true}' 150 | grep -E '\"state\"|errorText' | tail -n 3
        rm -f /media/developer/emu-install.ipk
        [ -d $APP ] || { echo 'not installed'; exit 1; }
        echo XDG_DATA_DIRS=$APP/firefox-runtime/share > $APP/env
        [ $WEBOS = 6 ] && echo WEBOS_XDG_AS_WEBOS4=1 >> $APP/env
        # 6.0's sam runs apps as root; the TV runs them as the app's user.
        # The jailer stand-in, as geckotv, does that for geckotv.real.
        if [ $WEBOS = 6 ]; then
            mv $APP/geckotv $APP/geckotv.real && cp /usr/bin/jailer $APP/geckotv
        fi
        ls -ld $APP $APP/geckotv" ;;
launch)
    ssh "${SSH_OPTS[@]}" -p 6623 root@127.0.0.1 "
        ln -sf $APP/firefox-runtime/ld-linux.so.2 /tmp/ld32.so
        P=\$(pidof firefox); [ -z \"\$P\" ] || { kill \$P; sleep 2; }
        luna-send -n 1 -f luna://com.webos.applicationManager/launch '{\"id\":\"com.github.gprot42.geckotv\"}' > /tmp/ls.out 2>&1 & p=\$!
        sleep 5; kill \$p 2>/dev/null; grep -E 'returnValue|errorText' /tmp/ls.out" ;;
*)
    sed -n '2,19p' "$0"; exit 2 ;;
esac
