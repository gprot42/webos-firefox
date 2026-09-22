#!/bin/bash
# Build the probe five ways inside the ffbuild container (as user builder):
#   podman exec -u builder ffbuild bash /src/experiments/rust-softfp/build.sh
# Output binaries land in out/, ready to copy to the TV.
set -euo pipefail
export RUSTUP_HOME=/work/rustup CARGO_HOME=/work/cargo PATH=/work/cargo/bin:$PATH
cd "$(dirname "$0")"
export CARGO_TARGET_DIR=/work/softfp-target
mkdir -p out
build() {  # name toolchain target linker [extra cargo args...]
    local name=$1 tc=$2 target=$3 linker=$4; shift 4
    local tvar; tvar=$(basename "$target" .json | tr 'a-z-' 'A-Z_')
    echo "=== $name"
    env "CARGO_TARGET_${tvar}_LINKER=$linker" PROBE_BUILD="$name" \
        cargo "+$tc" build --release --target "$target" "$@" 2>&1 | grep -vE '^\s+(Compiling|Finished)' || true
    cp "$CARGO_TARGET_DIR/$(basename "$target" .json)/release/softfp-probe" "out/$name"
}
build soft-stock      stable  armv7-unknown-linux-gnueabi    arm-linux-gnueabi-gcc
RUSTFLAGS="-C target-feature=-soft-float,+vfp3,+neon" \
build softfp-stable   stable  armv7-unknown-linux-gnueabi    arm-linux-gnueabi-gcc
build softfp-custom   nightly "$PWD/armv7-webos-linux-gnueabi.json" arm-linux-gnueabi-gcc -Zbuild-std=std,panic_abort -Zjson-target-spec
build hardfp-static   stable  armv7-unknown-linux-musleabihf arm-linux-gnueabihf-gcc
build arm64-static    stable  aarch64-unknown-linux-musl     cc
echo "=== results"
for f in out/*; do
    printf '%-16s %s\n' "$(basename "$f")" "$(file -b "$f" | cut -d, -f1-2)"
    readelf -A "$f" 2>/dev/null | grep -E "Tag_ABI_VFP_args|Tag_FP_arch|Tag_Advanced_SIMD" | sed 's/^/    /' || true
    objdump -T "$f" 2>/dev/null | grep -oE 'GLIBC_[0-9.]+' | sort -Vu | tail -1 | sed 's/^/    newest glibc symbol: /' || true
done
