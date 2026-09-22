#!/bin/bash
# Native aarch64 Firefox ESR 153 build inside the Ubuntu container.
# The TV userspace is 32-bit softfp. Rust has no softfp target, so the
# browser is aarch64 and runs through org.webosbrew.bridge-64to32, whose
# bundled glibc is 2.39, the same version as Ubuntu 24.04.
set -euo pipefail
trap 'echo BUILD_FAILED' ERR

WORK=/work
SRC=/src
TAG=$(cat "$SRC/build/FIREFOX_TAG")
TREE=$WORK/firefox
OBJ=$WORK/obj-firefox
RUNTIME=$SRC/app/firefox-runtime
BRIDGE_LIB=/media/developer/apps/usr/palm/applications/org.webosbrew.bridge-64to32/lib
INTERP=$BRIDGE_LIB/ld-linux-aarch64.so.1
RPATH="\$ORIGIN:$BRIDGE_LIB"
LOG=$SRC/build/linux-build.log

mkdir -p "$SRC/build" "$SRC/app" "$SRC/dist"
exec > >(tee -a "$LOG") 2>&1

phase() { echo; echo "===== $(date -Is) $* ====="; }

if [[ "${1:-}" != "--as-user" ]]; then
    phase "install host packages"
    export DEBIAN_FRONTEND=noninteractive
    apt-get update
    apt-get install -y --no-install-recommends \
        build-essential python3 python3-venv python3-pip python3-dev \
        git curl ca-certificates xz-utils zip unzip pkg-config cmake nodejs \
        meson ninja-build clang llvm libclang-dev llvm-dev \
        libgtk-3-dev libdbus-1-dev libpulse-dev libasound2-dev \
        patchelf file rsync gawk bison flex wget gnupg ca-certificates
    # ESR 153 rejects clang older than 19. Ubuntu 24.04 ships 18.
    if ! command -v clang-19 >/dev/null 2>&1; then
        curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key \
            | gpg --dearmor -o /usr/share/keyrings/llvm-archive-keyring.gpg
        echo "deb [signed-by=/usr/share/keyrings/llvm-archive-keyring.gpg] http://apt.llvm.org/noble/ llvm-toolchain-noble-19 main" \
            > /etc/apt/sources.list.d/llvm-19.list
        apt-get update
        apt-get install -y --no-install-recommends clang-19 lld-19 libclang-19-dev
    fi
    if ! id builder >/dev/null 2>&1; then
        useradd --create-home builder
    fi
    mkdir -p "$WORK"
    chown -R builder:builder "$WORK"
    chmod -R a+rwx "$SRC/app" "$SRC/build" "$SRC/dist"
    phase "drop privileges"
    exec su builder -s /bin/bash -c "bash $SRC/build/linux-build.sh --as-user"
fi

export RUSTUP_HOME=$WORK/rustup
export CARGO_HOME=$WORK/cargo
export PATH="$CARGO_HOME/bin:$PATH"
export CC=clang-19
export CXX=clang++-19
export MOZBUILD_STATE_PATH=$WORK/mozbuild
mkdir -p "$RUSTUP_HOME" "$CARGO_HOME" "$MOZBUILD_STATE_PATH"

phase "rustup"
if [[ ! -x $CARGO_HOME/bin/rustup ]]; then
    curl --proto '=https' --tlsv1.2 -fsSL https://sh.rustup.rs | sh -s -- -y --default-toolchain stable
fi
rustup show

phase "firefox source $TAG"
if [[ ! -d $TREE/.git ]]; then
    git clone --depth 1 --branch "$TAG" https://github.com/mozilla-firefox/firefox.git "$TREE"
fi

cat > "$TREE/mozconfig" <<EOF
ac_add_options --enable-application=browser
ac_add_options --enable-default-toolkit=cairo-gtk3-wayland-only
ac_add_options --disable-updater
ac_add_options --disable-crashreporter
ac_add_options --disable-tests
ac_add_options --disable-debug
ac_add_options --disable-debug-symbols
ac_add_options --enable-optimize
ac_add_options --enable-release
ac_add_options --without-wasm-sandboxed-libraries
# Size: a TV has no camera, microphone, printer or screen reader, so none of
# this code ever runs. Together these cut libxul, which is 159 MB of the
# 321 MB installed footprint.
ac_add_options --disable-webrtc
ac_add_options --disable-accessibility
ac_add_options --disable-printing
ac_add_options --disable-parental-controls
ac_add_options --disable-webspeech
ac_add_options --disable-synth-speechd
ac_add_options --disable-profiling
ac_add_options --disable-geckodriver
ac_add_options --enable-strip
ac_add_options --enable-install-strip
ac_add_options --with-libclang-path=/usr/lib/llvm-19/lib
ac_add_options --with-clang-path=/usr/bin/clang-19
mk_add_options MOZ_OBJDIR=$OBJ
mk_add_options AUTOCLOBBER=1
EOF

phase "cbindgen"
if ! cbindgen --version >/dev/null 2>&1; then
    cargo install cbindgen --version 0.29.4
fi
cbindgen --version

phase "mach build"
cd "$TREE"
./mach build

phase "package firefox"
cd "$TREE"
./mach package
TARBALL=$(echo "$OBJ"/dist/firefox-*.linux-aarch64.tar.xz)
phase "assemble runtime from $TARBALL"
rm -rf "$RUNTIME" "$WORK/firefox-unpack"
mkdir -p "$RUNTIME" "$WORK/firefox-unpack"
tar -xJf "$TARBALL" -C "$WORK/firefox-unpack"
cp -a "$WORK/firefox-unpack/firefox/." "$RUNTIME/"

copy_deps() {
    local bin=$1
    ldd "$bin" 2>/dev/null | awk '/=> \// {print $1, $3}' | while read -r soname lib; do
        [[ -n $lib && -f $lib ]] || continue
        case $soname in
            libc.so.6|libm.so.6|libdl.so.2|libpthread.so.0|librt.so.1|libresolv.so.2|libutil.so.1|ld-linux-aarch64.so.1)
                continue
                ;;
        esac
        cp -L "$lib" "$RUNTIME/$soname" || true
    done
}

# Repeat so newly copied libraries pull their own dependencies.
for _ in 1 2 3 4 5 6; do
    while IFS= read -r bin; do
        copy_deps "$bin"
    done < <(find "$RUNTIME" -type f -name '*.so' -o -type f -name 'firefox' -o -type f -name 'firefox-bin')
done

while IFS= read -r bin; do
    file -b "$bin" | grep -q ELF || continue
    patchelf --set-rpath "$RPATH" "$bin" || true
    if patchelf --print-interpreter "$bin" >/dev/null 2>&1; then
        patchelf --set-interpreter "$INTERP" "$bin"
    fi
done < <(find "$RUNTIME" -type f)

mkdir -p "$RUNTIME/defaults/pref"
cp "$SRC/app/defaults/pref/00-webos.js" "$RUNTIME/defaults/pref/"
chmod 755 "$RUNTIME/firefox" "$RUNTIME/firefox-bin" 2>/dev/null || true
python3 - <<PY
import json
from pathlib import Path
p = Path("$SRC/app/appinfo.json")
info = json.loads(p.read_text())
info["requiredMemory"] = 400
info["appDescription"] = "Firefox ESR 153 for webOS TV, aarch64 via the 64-bit bridge."
p.write_text(json.dumps(info, indent=2) + "\n")
PY

phase "package ipk"
python3 "$SRC/scripts/pack-ipk.py"
echo "BUILD_DONE $(date -Is)"
