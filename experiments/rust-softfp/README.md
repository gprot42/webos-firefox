# rust-softfp

Tests whether Rust can target the webOS TV's 32-bit softfp userspace at full
floating-point speed. Results are in `findings.md`, section 12.

```sh
podman exec -u builder ffbuild bash /src/experiments/rust-softfp/build.sh
scp out/* root@TV:/tmp/rustfp/
ssh root@TV 'cd /tmp/rustfp && XDG_RUNTIME_DIR=/tmp/xdg WAYLAND_DISPLAY=wayland-0 ./softfp-custom --gpu'
```

`--gpu` renders offscreen through the TV's EGL and GLES and reads a pixel
back; nothing appears on screen. The five builds are stock soft-float, stable
with FPU features, the custom `armv7-webos-linux-gnueabi.json` target, and
static hardfp and 64-bit baselines.
