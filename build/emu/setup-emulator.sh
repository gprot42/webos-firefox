#!/bin/bash
# Set up LG's webOS TV 4.0 emulator for testing Firefox (see README.md here):
# download it, convert its disk for QEMU, and add root SSH access for testing.
# Run on the Mac, with the ffbuild container running (debugfs runs there).
#   build/emu/setup-emulator.sh            # into $EMU_DIR, default ~/webos4-emulator
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
set -euo pipefail
EMU_DIR=${EMU_DIR:-$HOME/webos4-emulator}
URL=https://archive.org/download/lg-webos-tv-emulator/Emulator_tv_linux_v4.0.0.zip
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
GEN=$ROOT/build/gen           # git-ignored, and /src/build/gen in ffbuild
IMG=$EMU_DIR/webos4.raw

for t in qemu-img qemu-system-i386 podman ssh-keygen unzip curl python3; do
    command -v "$t" >/dev/null || { echo "missing: $t"; exit 1; }
done
podman exec ffbuild true 2>/dev/null || { echo "start the ffbuild container first"; exit 1; }
podman exec ffbuild bash -c 'command -v debugfs >/dev/null || apt-get install -y -q e2fsprogs >/dev/null'
if [ -e "$IMG" ]; then
    echo "$IMG exists; move it away to set up a new emulator"; exit 1
fi
mkdir -p "$EMU_DIR" "$GEN"

# 1. The emulator's disk.
ZIP=${ZIP:-$EMU_DIR/Emulator_tv_linux_v4.0.0.zip}
if [ ! -f "$ZIP" ]; then
    echo "downloading the emulator (1.26 GB)"
    curl -fL --retry 3 -o "$ZIP" "$URL"
fi
unzip -q -o -j "$ZIP" Emulator/v4.0.0/LG_webOS_TV_Emulator.vmdk -d "$EMU_DIR"
qemu-img convert -O raw "$EMU_DIR/LG_webOS_TV_Emulator.vmdk" "$IMG"
rm "$EMU_DIR/LG_webOS_TV_Emulator.vmdk"

# 2. A key for root, used by emu.sh.
[ -f "$EMU_DIR/root_key" ] ||
    ssh-keygen -q -t rsa -b 2048 -m PEM -N "" -C "webos4-emulator-test" -f "$EMU_DIR/root_key"
cp "$EMU_DIR/root_key.pub" "$GEN/emu_authorized_keys"

# 3. The Developer Mode start script.
cat > "$GEN/start-devmode.sh" <<'EOS'
#!/bin/sh
# Added for testing Firefox on this local QEMU copy of LG's webOS TV 4.0
# emulator (build/emu/setup-emulator.sh). LG's devmode job runs this Developer
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
podman exec -i ffbuild bash -s <<'EOC'
set -euo pipefail
G=/src/build/gen
D=/cryptofs/apps/usr/palm/services/com.palmdts.devmode.service
run() { debugfs -w -R "$2" "$1" 2>&1 | grep -v "^debugfs " || true; }
own() { run "$1" "sif $2 uid 0"; run "$1" "sif $2 gid 0"; run "$1" "sif $2 mode $3"; }
for p in p2 p4; do e2fsck -fy "$G/$p.img" >/dev/null 2>&1 || true; done
run "$G/p2.img" "mkdir /emu-test"; own "$G/p2.img" /emu-test 040755
run "$G/p2.img" "write $G/emu_authorized_keys /emu-test/authorized_keys"
own "$G/p2.img" /emu-test/authorized_keys 0100644
for d in /cryptofs/apps /cryptofs/apps/usr /cryptofs/apps/usr/palm /cryptofs/apps/usr/palm/services $D; do
    run "$G/p4.img" "mkdir $d"; own "$G/p4.img" "$d" 040755
done
run "$G/p4.img" "write $G/start-devmode.sh $D/start-devmode.sh"
own "$G/p4.img" "$D/start-devmode.sh" 0100700
for p in p2 p4; do
    e2fsck -fn "$G/$p.img" >/dev/null 2>&1 || { echo "$p.img: filesystem check failed"; exit 1; }
done
key=$(debugfs -R "cat /emu-test/authorized_keys" "$G/p2.img" 2>/dev/null)
mode=$(debugfs -R "stat $D/start-devmode.sh" "$G/p4.img" 2>/dev/null)
[[ $key == *webos4-emulator-test* && $mode == *"Mode:  0700"* ]] || { echo "files missing"; exit 1; }
echo "files added, filesystems clean"
EOC
dd if="$GEN/p2.img" of="$IMG" bs=512 seek="$P2_START" conv=notrunc 2>/dev/null
dd if="$GEN/p4.img" of="$IMG" bs=512 seek="$P4_START" conv=notrunc 2>/dev/null
rm -f "$GEN/p2.img" "$GEN/p4.img" "$GEN/emu_authorized_keys" "$GEN/start-devmode.sh"
echo "emulator ready in $EMU_DIR"
echo "next: build/emu/emu.sh start, wait about 90 s, then build/emu/emu.sh ssh id"
