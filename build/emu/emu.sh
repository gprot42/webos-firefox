#!/bin/bash
# Drive the local QEMU copy of LG's webOS TV 4.0 emulator, used to test
# Firefox on webOS 4's compositor. One entry point, so a single permission
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
# EMU_DIR holds the disk image (webos4.raw) and root_key, as set up by
# build/emu/setup-emulator.sh; see build/emu/README.md.
set -euo pipefail
EMU_DIR=${EMU_DIR:-$HOME/webos4-emulator}
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
    # Settings follow LG_webOS_TV_Emulator.vbox: PIIX IDE disk, PCnet NIC with
    # the SDK's port forwards (host 127.0.0.1 only), USB tablet, AC97, and a
    # plain VGA framebuffer (the VM has 3D acceleration off).
    qemu-system-i386 -name webos4 \
      -machine pc -accel tcg,thread=multi,tb-size=1024 -cpu max -smp 3 -m 2048 \
      -drive file="$EMU_DIR/webos4.raw",format=raw,if=ide,cache=writeback \
      -netdev user,id=n0,hostfwd=tcp:127.0.0.1:6622-:22,hostfwd=tcp:127.0.0.1:6623-:2222,hostfwd=tcp:127.0.0.1:9998-:9998,hostfwd=tcp:127.0.0.1:19001-:19001 \
      -device pcnet,netdev=n0,mac=08:00:27:a2:0d:39 \
      -device pci-ohci,id=ohci -device usb-tablet,bus=ohci.0 \
      -audiodev none,id=snd0 -device AC97,audiodev=snd0 \
      -device VGA,vgamem_mb=32 \
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
    qmp_input "{\"type\":\"abs\",\"data\":{\"axis\":\"x\",\"value\":$x}},{\"type\":\"abs\",\"data\":{\"axis\":\"y\",\"value\":$y}}" \
              '{"type":"btn","data":{"down":true,"button":"left"}}' \
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
*)
    sed -n '2,17p' "$0"; exit 2 ;;
esac
