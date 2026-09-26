#!/bin/bash
# Set up LG's webOS TV 4.0 or 6.0 emulator for testing Firefox (see README.md):
# download it, convert its disk for QEMU, and add root SSH access for testing.
# Run on the Mac, with the ffbuild container running (debugfs runs there).
#   build/emu/setup-emulator.sh            # into $EMU_DIR, default ~/webos4-emulator
#   WEBOS=6 build/emu/setup-emulator.sh    # 6.0, default ~/webos6-emulator
#   ZIP=/path/Emulator_tv_linux_v4.0.0.zip build/emu/setup-emulator.sh
# Afterwards: build/emu/emu.sh start, wait about 90 s, build/emu/emu.sh ssh id.
#
# Root access: LG's dropbear refuses root, and the system sits on an encrypted
# partition whose unlock fails if the boot scripts or /etc/init on the plain
# root partition change. So nothing there is changed; two files are added:
#  - /emu-test/authorized_keys on the root partition (hda2), a new key's
#    public half;
#  - start-devmode.sh on the unencrypted media partition (hda4), the Developer
#    Mode start script LG's devmode job runs as root at boot, after the unlock.
#    It installs that key for root and starts a second dropbear on port 2222
#    that takes keys only; emu.sh forwards it to 127.0.0.1:6623.
#
# 6.0 also needs OpenGL for its compositor, which VirtualBox gives it through
# VMware SVGA 3D and QEMU on this Mac cannot (no host 3D). So a software
# driver goes onto the media partition as /developer/mesa-sw: Mesa's
# kms_swrast from Debian 10 (glibc 2.28, as in the image) with the libraries
# the image lacks, and start-devmode.sh points the compositor at it through
# its optional environment file. emu.sh gives 6.0 a virtio-vga display.
set -euo pipefail
WEBOS=${WEBOS:-4}
case $WEBOS in 4|6) ;; *) echo "WEBOS must be 4 or 6"; exit 1 ;; esac
EMU_DIR=${EMU_DIR:-$HOME/webos$WEBOS-emulator}
URL=https://archive.org/download/lg-webos-tv-emulator/Emulator_tv_linux_v$WEBOS.0.0.zip
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
GEN=$ROOT/build/gen           # git-ignored, and /src/build/gen in ffbuild
IMG=$EMU_DIR/webos$WEBOS.raw

for t in qemu-img qemu-system-i386 podman ssh-keygen ssh-keyscan unzip curl python3; do
    command -v "$t" >/dev/null || { echo "missing: $t"; exit 1; }
done
podman exec ffbuild true 2>/dev/null || { echo "start the ffbuild container first"; exit 1; }
podman exec ffbuild bash -c 'command -v debugfs >/dev/null || apt-get install -y -q e2fsprogs >/dev/null'
if [ -e "$IMG" ]; then
    echo "$IMG exists; move it away to set up a new emulator"; exit 1
fi
mkdir -p "$EMU_DIR" "$GEN"

# 1. The emulator's disk.
ZIP=${ZIP:-$EMU_DIR/Emulator_tv_linux_v$WEBOS.0.0.zip}
if [ ! -f "$ZIP" ]; then
    echo "downloading the emulator (1.3 GB)"
    curl -fL --retry 3 -o "$ZIP" "$URL"
fi
unzip -q -o -j "$ZIP" Emulator/v$WEBOS.0.0/LG_webOS_TV_Emulator.vmdk -d "$EMU_DIR"
qemu-img convert -O raw "$EMU_DIR/LG_webOS_TV_Emulator.vmdk" "$IMG"
rm "$EMU_DIR/LG_webOS_TV_Emulator.vmdk"

# 2. A key for root, used by emu.sh.
[ -f "$EMU_DIR/root_key" ] ||
    ssh-keygen -q -t rsa -b 2048 -m PEM -N "" -C "webos$WEBOS-emulator-test" -f "$EMU_DIR/root_key"
cp "$EMU_DIR/root_key.pub" "$GEN/emu_authorized_keys"

# 3. The Developer Mode start script.
cat > "$GEN/start-devmode.sh" <<'EOS'
#!/bin/sh
# Added for testing Firefox on this local QEMU copy of LG's webOS TV emulator
# (build/emu/setup-emulator.sh). LG's devmode job runs this Developer
# Mode script as root at boot, after the encrypted system is mounted.
if [ -f /emu-test/authorized_keys ]; then
    mkdir -p /home/root/.ssh
    cp /emu-test/authorized_keys /home/root/.ssh/authorized_keys
    chown 0:0 /home/root /home/root/.ssh /home/root/.ssh/authorized_keys
    chmod 700 /home/root /home/root/.ssh
    chmod 600 /home/root/.ssh/authorized_keys
    # LG's dropbear refuses root (-w). This one lets root in with that key
    # only (-s: no password logins); QEMU forwards it to the host's
    # 127.0.0.1:6623.
    /usr/sbin/dropbear -s -r /var/lib/dropbear/dropbear_rsa_host_key -p 2222
fi
EOS
if [ "$WEBOS" = 6 ]; then
    cat >> "$GEN/start-devmode.sh" <<'EOS'
# 6.0 in QEMU: software OpenGL for the compositor (see setup-emulator.sh). The
# file is read by surface-manager-daemon.service, which is restarted once.
M=/media/developer/mesa-sw
F=/var/systemd/system/env/surface-manager.env
if [ -f $M/kms_swrast_dri.so ] && [ ! -f $F ]; then
    mkdir -p "${F%/*}"
    printf '%s\n' "LIBGL_DRIVERS_PATH=$M" "GBM_DRIVERS_PATH=$M" "LD_LIBRARY_PATH=$M" > $F
    systemctl restart surface-manager-daemon
fi
EOS

    # 3b. The software OpenGL driver, from Debian 10's archive (about 21 MB).
    DEBS="mesa/libgl1-mesa-dri_18.3.6-2+deb10u1 llvm-toolchain-7/libllvm7_7.0.1-8+deb10u2
          lm-sensors/libsensors5_3.5.0-3 elfutils/libelf1_0.176-1.1
          libdrm/libdrm-amdgpu1_2.4.97-1 libedit/libedit2_3.1-20181209-1
          ncurses/libtinfo6_6.1+20181013-2+deb10u2 libbsd/libbsd0_0.9.1-2+deb10u1"
    mkdir -p "$EMU_DIR/mesa-deb10" "$GEN/deb10"
    for d in $DEBS; do
        src=${d%%/*} deb=${d#*/}_i386.deb
        case $src in lib?*) pool=${src:0:4} ;; *) pool=${src:0:1} ;; esac
        [ -f "$EMU_DIR/mesa-deb10/$deb" ] ||
            curl -fsSL --retry 3 -o "$EMU_DIR/mesa-deb10/$deb" \
                "https://archive.debian.org/debian/pool/main/$pool/$src/$deb"
        cp "$EMU_DIR/mesa-deb10/$deb" "$GEN/deb10/"
    done
    # libatomic (for LLVM) comes from the i686 build sysroot: it needs only
    # glibc 2.1.3.
    podman exec ffbuild bash -c 'set -e; cd /src/build/gen/deb10
        rm -rf x mesa-sw; mkdir mesa-sw
        for f in *.deb; do dpkg-deb -x "$f" x; done
        L=x/usr/lib/i386-linux-gnu
        cp -L $L/dri/kms_swrast_dri.so $L/libLLVM-7.so.1 $L/libsensors.so.5 \
              $L/libedit.so.2 mesa-sw/
        for l in libelf.so.1 libdrm_amdgpu.so.1 libtinfo.so.6 libbsd.so.0; do
            cp -L $(ls $L/$l x/lib/i386-linux-gnu/$l 2>/dev/null | head -1) mesa-sw/
        done
        cp -L /work/sysroot-i386/usr/lib/i386-linux-gnu/libatomic.so.1 mesa-sw/
        rm -rf x'
fi

# 4. Partitions 2 (root) and 4 (media), from the disk's partition table.
read -r P2_START P2_SIZE P4_START P4_SIZE < <(python3 - "$IMG" <<'EOP'
import struct, sys
with open(sys.argv[1], "rb") as f:
    mbr = f.read(512)
assert mbr[510:512] == b"\x55\xaa", "no partition table"
parts = [struct.unpack_from("<II", mbr, 0x1be + 16 * i + 8) for i in range(4)]
print(parts[1][0], parts[1][1], parts[3][0], parts[3][1])
EOP
)
echo "root partition at sector $P2_START, media partition at sector $P4_START"
dd if="$IMG" of="$GEN/p2.img" bs=512 skip="$P2_START" count="$P2_SIZE" 2>/dev/null
dd if="$IMG" of="$GEN/p4.img" bs=512 skip="$P4_START" count="$P4_SIZE" 2>/dev/null

# 5. Add the two files.
podman exec -i -e WEBOS="$WEBOS" ffbuild bash -s <<'EOC'
set -euo pipefail
G=/src/build/gen
D=/cryptofs/apps/usr/palm/services/com.palmdts.devmode.service
run() { debugfs -w -R "$2" "$1" 2>&1 | grep -v "^debugfs " || true; }
own() { run "$1" "sif $2 uid 0"; run "$1" "sif $2 gid 0"; run "$1" "sif $2 mode $3"; }
for p in p2 p4; do e2fsck -fy "$G/$p.img" >/dev/null 2>&1 || true; done
run "$G/p2.img" "mkdir /emu-test"; own "$G/p2.img" /emu-test 040755
run "$G/p2.img" "write $G/emu_authorized_keys /emu-test/authorized_keys"
own "$G/p2.img" /emu-test/authorized_keys 0100644
# 6.0's media partition is empty until its first boot creates /cryptofs.
for d in /cryptofs /cryptofs/apps /cryptofs/apps/usr /cryptofs/apps/usr/palm /cryptofs/apps/usr/palm/services $D; do
    run "$G/p4.img" "mkdir $d"; own "$G/p4.img" "$d" 040755
done
run "$G/p4.img" "write $G/start-devmode.sh $D/start-devmode.sh"
own "$G/p4.img" "$D/start-devmode.sh" 0100700
if [ "$WEBOS" = 6 ]; then
    for d in /developer /developer/mesa-sw; do
        run "$G/p4.img" "mkdir $d"; own "$G/p4.img" "$d" 040755
    done
    for f in "$G"/deb10/mesa-sw/*; do
        run "$G/p4.img" "write $f /developer/mesa-sw/${f##*/}"
        own "$G/p4.img" "/developer/mesa-sw/${f##*/}" 0100644
    done
fi
for p in p2 p4; do
    e2fsck -fn "$G/$p.img" >/dev/null 2>&1 || { echo "$p.img: filesystem check failed"; exit 1; }
done
key=$(debugfs -R "cat /emu-test/authorized_keys" "$G/p2.img" 2>/dev/null)
mode=$(debugfs -R "stat $D/start-devmode.sh" "$G/p4.img" 2>/dev/null)
[[ $key == *webos$WEBOS-emulator-test* && $mode == *"Mode:  0700"* ]] || { echo "files missing"; exit 1; }
echo "files added, filesystems clean"
EOC
dd if="$GEN/p2.img" of="$IMG" bs=512 seek="$P2_START" conv=notrunc 2>/dev/null
dd if="$GEN/p4.img" of="$IMG" bs=512 seek="$P4_START" conv=notrunc 2>/dev/null
rm -rf "$GEN/p2.img" "$GEN/p4.img" "$GEN/emu_authorized_keys" "$GEN/start-devmode.sh" "$GEN/deb10"
if [ "$WEBOS" = 6 ]; then
    # 6. 6.0's first boot sets up the media partition and does not run the
    # Developer Mode script yet; later boots do. So boot it once here.
    echo "first boot (a few minutes)"
    E="env WEBOS=6 EMU_DIR=$EMU_DIR $ROOT/build/emu/emu.sh"
    $E start >/dev/null
    for _ in $(seq 1 80); do
        ssh-keyscan -p 6622 -T 3 127.0.0.1 2>/dev/null | grep -q ssh && break
        sleep 5
    done
    sleep 60
    $E stop >/dev/null
fi
echo "emulator ready in $EMU_DIR"
[ "$WEBOS" = 6 ] && p="WEBOS=6 " || p=
echo "next: ${p}build/emu/emu.sh start, wait about 90 s, then ${p}build/emu/emu.sh ssh id"
