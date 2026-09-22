#!/bin/bash
# Firefox ESR 153 as a native 32-bit program (ARMv7, NEON, softfp) for the
# webOS TV, built inside the Ubuntu container. Run as root; it installs the
# host packages, builds the Debian armel sysroot if missing, then drops to the
# builder user for Rust, the Firefox source, the patches and the build.
#   podman exec ffbuild bash /src/build/linux-build.sh
# Then: build/build-adapter.sh and build/assemble-runtime.sh.
set -euo pipefail
trap 'echo BUILD_FAILED' ERR

WORK=/work
SRC=/src
TAG=$(cat "$SRC/build/FIREFOX_TAG")
TREE=$WORK/firefox
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
        file rsync gawk bison flex wget gnupg libwayland-bin
    # ESR 153 rejects clang older than 19. Ubuntu 24.04 ships 18.
    if ! command -v clang-19 >/dev/null 2>&1; then
        curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key \
            | gpg --dearmor -o /usr/share/keyrings/llvm-archive-keyring.gpg
        echo "deb [signed-by=/usr/share/keyrings/llvm-archive-keyring.gpg] http://apt.llvm.org/noble/ llvm-toolchain-noble-19 main" \
            > /etc/apt/sources.list.d/llvm-19.list
        apt-get update
        apt-get install -y --no-install-recommends clang-19 lld-19 libclang-19-dev
    fi
    if [[ ! -f $WORK/sysroot-armel/lib/arm-linux-gnueabi/libc.so.6 ]]; then
        phase "armel sysroot"
        bash "$SRC/build/sysroot-armel.sh"
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
export MOZBUILD_STATE_PATH=$WORK/mozbuild
export MOZCONFIG=$SRC/build/mozconfig-arm32
mkdir -p "$RUSTUP_HOME" "$CARGO_HOME" "$MOZBUILD_STATE_PATH"

phase "rustup"
if [[ ! -x $CARGO_HOME/bin/rustup ]]; then
    curl --proto '=https' --tlsv1.2 -fsSL https://sh.rustup.rs | sh -s -- -y --default-toolchain stable
fi
rustup target add armv7-unknown-linux-gnueabi
rustup show

phase "cbindgen"
if ! cbindgen --version >/dev/null 2>&1; then
    cargo install cbindgen --version 0.29.4
fi
cbindgen --version

phase "firefox source $TAG"
if [[ ! -d $TREE/.git ]]; then
    git clone --depth 1 --branch "$TAG" https://github.com/mozilla-firefox/firefox.git "$TREE"
fi

phase "patch firefox"
cd "$TREE"
for patch in "$SRC"/build/patches/*.patch; do
    if git -c safe.directory="$TREE" apply --check "$patch" 2>/dev/null; then
        git -c safe.directory="$TREE" apply "$patch" && echo "applied $(basename "$patch")"
    elif git -c safe.directory="$TREE" apply --reverse --check "$patch" 2>/dev/null; then
        echo "already applied $(basename "$patch")"
    else
        echo "PATCH DOES NOT APPLY: $(basename "$patch")"; exit 1
    fi
done

phase "configure"
./mach configure
phase "build"
./mach build
phase "package"
./mach package
ls -l "$WORK"/obj-arm32/dist/firefox-*.tar.xz
echo "BUILD_DONE $(date -Is)"
