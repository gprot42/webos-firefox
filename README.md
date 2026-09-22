# Firefox for webOS TV

Native browser for the OLED55C56LB (webOSTV 25, platform 10.3.1, firmware 33.31.68.01). Userspace is 32-bit ARM, softfp, glibc 2.35. The on-TV name is Firefox. The app id stays `com.github.gprot42.geckotv`, so an existing install upgrades in place.

Firefox is a trademark of the Mozilla Foundation. This packages an unmodified Firefox ESR build for personal use on one TV and ships its own icon artwork, not Mozilla's.

`app/geckotv.sh` runs `app/firefox-runtime/firefox` when that file is present. That runtime is Firefox ESR 153.3, built as 64-bit ARM. Rust has no softfp target, and this TV's libraries are softfp, so a 32-bit Firefox cannot call them. The binary uses the `org.webosbrew.bridge-64to32` loader. Install that bridge before launching Gecko. The highest glibc symbol in this build is GLIBC_2.38. The bridge ships glibc 2.39.

Until the runtime exists, the same launcher runs `app/smoke`, a Wayland client that paints a fullscreen GLES frame.

`findings.md` records what was measured on the device: the compositor behaviour behind the window bugs, why each fix is shaped the way it is, the video and memory numbers, and the diagnostic traps worth knowing before debugging this thing.

## Build the smoke IPK

The compiler is the webOS SDK at `/Users/aicoder/toolchains/arm-webos-linux-gnueabi_sdk-buildroot` (GCC 12.2). Its sysroot glibc is 2.12.2, which is old enough for this small client and the wrong sysroot for Firefox.

```sh
make
python3 scripts/pack-ipk.py
```

`scripts/install2tvfrommacos.sh` builds, packages, and installs to `root@192.168.0.79` with `~/.ssh/webos_deploy` when the TV answers. Ares device name `webos`.

After launch, the log is on the TV at:

```
/media/developer/apps/usr/palm/applications/com.github.gprot42.geckotv/geckotv.log
```

## Window size

The adapter tells Firefox its window is 1280x720 and the compositor scales it to the panel. There is no GPU path for this 64-bit build, so every pixel is rasterised and composited on the CPU and copied through shared memory; at 1080p that alone cost the parent process ~170% CPU and dropped a quarter of video frames, at 720p it is ~80% and under 1%. `WEBOS_XDG_SIZE=1920x1080` in the `env` file restores full resolution.

## Codecs

Firefox's bundled codec library only carries VP8, VP9 and Opus. H.264 and AAC come from FFmpeg, which Firefox loads at runtime as `libavcodec.so.60`. `build/ffmpeg-mini.sh` builds a 2.7 MB FFmpeg with just the H.264, AAC and MP3 decoders and copies it into `app/firefox-runtime`; `scripts/push-fix.sh` ships it. `about:support` then lists `H264 SWDEC` and `AAC SWDEC`. There is no hardware video decoding: the TV has no VA-API driver, so every frame is decoded on the CPU.

## Reading a crash

`webos-xdg: signal 11 addr=(nil)` with `g_log_structured_standard` and `gdk_event_source_check` in the backtrace is not a Firefox bug. GDK saw the Wayland socket die and Firefox's GLib log writer turned that into a `MOZ_CRASH`, which prints nothing in a release build. It means surface-manager restarted. Check `/var/log/reports/librdx` for the compositor's own report and the `NL_VSC` lines in `/var/log/messages*.gz` for which card LSM showed.

To capture the wire protocol for one run, create the marker file, launch, then delete it:

```
touch /media/developer/apps/usr/palm/applications/com.github.gprot42.geckotv/wayland-debug
```

The launcher then sets `WAYLAND_DEBUG=1`, and libwayland logs every request and event into `geckotv.log`.

## Firefox ESR 153

Pinned tag: `build/FIREFOX_TAG` (`FIREFOX_153_3_0esr_RELEASE`).

`build/linux-build.sh` compiles it in the Ubuntu container (`podman exec ffbuild`). `build/assemble-runtime.sh` turns `mach package` into `app/firefox-runtime` and `dist/com.github.gprot42.geckotv_0.1.0_arm.ipk`. The current IPK (0.1.3) is 110.5 MB. Installed, the runtime is about 324 MB. TV prefs are copied into the runtime from `app/defaults/pref/00-webos.js`. Firefox only scans `defaults/pref` and `defaults/preferences`; a file under `distribution/preferences` is never read. Sandbox environment variables are set in `geckotv.sh` because the process is already inside webOS `jailer`.
