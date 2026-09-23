# Firefox for webOS TV

Native browser for the OLED55C56LB (webOSTV 25, platform 10.3.1, firmware 33.31.68.01). Userspace is 32-bit ARM, softfp, glibc 2.35. The on-TV name is Firefox. The app id stays `com.github.gprot42.geckotv`, so an existing install upgrades in place.

Firefox is a trademark of the Mozilla Foundation. This packages a Firefox ESR build for personal use on one TV and ships its own icon artwork, not Mozilla's. The build carries three small build fixes in `build/patches/`.

`app/geckotv` runs `app/firefox-runtime/firefox` when that file is present. That runtime is Firefox ESR 153.3 built as a native 32-bit program (ARMv7, NEON, softfp), the same ABI as the TV's userspace. It runs on the TV's own glibc (2.30 or newer) and needs nothing else installed. libstdc++ is linked statically with its symbols hidden (`build/mozconfig-arm32`, `build/stdcxx-static/`), so Firefox does not depend on the TV's C++ library version; the Mali driver keeps using the TV's copy. Rust is built for the stock `armv7-unknown-linux-gnueabi` target with the FPU features switched on, so it uses the hardware FPU; `findings.md` section 12 records how that was proven on the TV. Firefox renders on the Mali-G52 through GPU WebRender. Scrolling a long page at 720p, it delivers 125 frames a second on about 105% of one core, where software rendering gives 88 frames on 125%. `findings.md` section 13 has the details and the four fixes GPU rendering needed.

An earlier 64-bit build ran through the `org.webosbrew.bridge-64to32` loader with software rendering only. It has been removed; `findings.md` keeps its measurements.

Until the runtime exists, the same launcher runs `app/smoke`, a Wayland client that paints a fullscreen GLES frame.

`findings.md` records what was measured on the device: the compositor behaviour behind the window bugs, why each fix is shaped the way it is, the video and memory numbers, and the diagnostic traps worth knowing before debugging this thing.

## Build the smoke IPK

The compiler is the webOS SDK at `/Users/aicoder/toolchains/arm-webos-linux-gnueabi_sdk-buildroot` (GCC 12.2). Its sysroot glibc is 2.12.2, which is old enough for this small client and the wrong sysroot for Firefox.

```sh
make
python3 scripts/pack-ipk.py
```

`scripts/install2tvfrommacos.sh` builds, packages, and installs to `root@192.168.0.79` with `~/.ssh/webos_deploy` when the TV answers. Ares device name `webos`.

To open a page at launch, pass it as the webOS `target` parameter (http and https only):

```sh
ares-launch -d webos com.github.gprot42.geckotv -p '{"target":"https://www.youtube.com/"}'
```

After launch, the log is on the TV at:

```
/media/developer/apps/usr/palm/applications/com.github.gprot42.geckotv/geckotv.log
```

## Window size

The adapter tells Firefox its window is 1280x720 and the compositor scales it to the panel. That was chosen when rendering was on the CPU: at 1080p the old software path cost the parent process ~170% CPU and dropped a quarter of video frames. `WEBOS_XDG_SIZE=1920x1080` in the `env` file restores full resolution.

## Codecs

Firefox loads the TV's own FFmpeg 5.0 (`libavcodec.so.59`), so `about:support` lists H.264, HEVC, VP8, VP9, AV1, AAC, MP3, Opus, Vorbis, FLAC and Wave as software decoders (SWDEC). No FFmpeg is bundled. There is no hardware video decoding: the TV has no VA-API driver, so every frame is decoded on the CPU.

## Remote control for debugging

Firefox starts without Marionette. To drive it from the Mac, create the marker file and relaunch:

```
touch /media/developer/apps/usr/palm/applications/com.github.gprot42.geckotv/marionette
```

Delete it afterwards. With Marionette on, `navigator.webdriver` is true on every page, and YouTube then stops sending video after about a minute and shows "Something went wrong"; see `findings.md` section 8.

## Reading a crash

`webos-xdg: signal 11 addr=(nil)` with `g_log_structured_standard` and `gdk_event_source_check` in the backtrace is not a Firefox bug. GDK saw the Wayland socket die and Firefox's GLib log writer turned that into a `MOZ_CRASH`, which prints nothing in a release build. It means surface-manager restarted. Check `/var/log/reports/librdx` for the compositor's own report and the `NL_VSC` lines in `/var/log/messages*.gz` for which card LSM showed.

To capture the wire protocol for one run, create the marker file, launch, then delete it:

```
touch /media/developer/apps/usr/palm/applications/com.github.gprot42.geckotv/wayland-debug
```

The launcher then sets `WAYLAND_DEBUG=1`, and libwayland logs every request and event into `geckotv.log`.

## Firefox ESR 153

Pinned tag: `build/FIREFOX_TAG` (`FIREFOX_153_3_0esr_RELEASE`).

`build/linux-build.sh` compiles it in the Ubuntu container (`podman exec ffbuild bash /src/build/linux-build.sh`). As root it installs the host packages and builds the Debian 11 armel sysroot (`build/sysroot-armel.sh`), then as the `builder` user it installs Rust, fetches the source, applies `build/patches/` and runs `mach build` and `mach package`. `build/build-adapter.sh` builds the adapter library. `build/assemble-runtime.sh` turns `mach package` into `app/firefox-runtime`, bundling the libraries the TV lacks, and packs `dist/com.github.gprot42.geckotv_<version>_arm.ipk`. Installed, the runtime is about 253 MB. TV prefs are copied into the runtime from `app/defaults/pref/00-webos.js`. Firefox only scans `defaults/pref` and `defaults/preferences`; a file under `distribution/preferences` is never read. Sandbox environment variables are set by the launcher because the process is already inside webOS `jailer`.
