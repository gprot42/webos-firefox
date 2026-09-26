# Findings

What was measured on the OLED55C56LB (webOS TV 25, platform 10.3.1, firmware
33.31.68.01) while getting Firefox ESR 153 usable, and why each fix is shaped
the way it is. Everything here was observed on the device, not inferred.
`README.md` covers how to build and install; this file covers why.

Dates are 2026-09-22.

---

## 1. The platform

| | |
|---|---|
| CPU | 4 x Cortex-A76 @ 1.4 GHz, NEON and dot-product |
| RAM | 2030 MB total, of which **692 MB is CMA** reserved for the video/GPU pipeline |
| Userspace | 32-bit ARM (the kernel is aarch64) |
| Swap | `/dev/f2io-0`, a 600 MB **flash** partition, `vm.swappiness=100` |
| GPU for us | none usable: no VA-API driver, no 64-bit EGL Firefox can load |

Userspace really has about 1.3 GB. Non-Firefox processes hold roughly 344 MB of
it, so the browser gets somewhere under 1 GB before the TV starts swapping to
flash. That constraint drives most of what follows.

Firefox runs as **uid 6784** inside the webOS jail, not root. Anything in
`geckotv.c` guarded by `geteuid() == 0` never executes in normal use, and
`/usr/bin/luna-send` is mode 0700 root so the app cannot call it either.

Background load is significant before Firefox starts: `servicemanager` and
`ls-hubd` together burn most of a core, plus `voxrelay` (a user daemon, ~23%)
and `voiceinput_preprocessor` (~12%). Load average sits at 15 to 19.

---

## 2. The compositor, and why an adapter exists at all

LSM (`luna-surfacemanager`) does not implement `xdg_shell`. Apps use `wl_shell`
plus `wl_webos_shell`. `src/webos-xdg.c` is compiled into `libwayland-client`
and translates Firefox's `xdg_shell` calls onto what the TV actually offers.

Two LSM behaviours explain nearly every window bug we hit. Both are visible in
the open-source tree at `github.com/webosose/luna-surfacemanager`:

1. **`surfaceCreated` builds a `WebOSSurfaceItem` for every `wl_surface`**, with
   `appId` empty and type defaulting to `_WEBOS_WINDOW_TYPE_CARD`.
2. **`FullscreenView.qml checkFullscreen` fullscreens any item** whose type is
   `_WEBOS_WINDOW_TYPE_CARD` and whose `displayAffinity` matches the display.
   It checks neither the shell role nor the parentage, and `m_displayAffinity`
   defaults to 0, which matches.

So *any* surface that receives content becomes a fullscreen card and minimises
the browser. That is the single root cause behind the original crash, the
nameless card, and the menu failure.

---

## 3. Fixed: the app vanishing on wheel or click

**Symptom.** Scrolling or clicking the Magic Remote made the app disappear,
sometimes leaving a `signal 11` record in `geckotv.log`.

**What the crash records actually are.** Every `webos-xdg: signal 11 addr=(nil)`
shares one backtrace: GDK's `gdk_event_source_check` (gdkeventsource.c:96) logs
`Error reading events from display`, Firefox's GLib log writer in
`toolkit/xre/nsSigHandlers.cpp` treats that as fatal and calls
`MOZ_CRASH_UNSAFE_PRINTF`, which stores to address 0. Release builds print
nothing first, which is why the message never appears. **It always means the
Wayland socket died, i.e. surface-manager restarted.** Check
`/var/log/reports/librdx` for the compositor's own report; one there showed it
segfaulting inside `WebOSSurfaceItem::setFullscreen(bool)`.

**Cause.** GDK attaches a buffer to its cursor surface on pointer enter. LSM
turned that surface into a nameless card and swapped the browser out.

**Fix.** The adapter records the surface named in `wl_pointer.set_cursor` and
drops every content request on it (attach, damage, commit, scale, regions).
The TV draws its own pointer, so nothing is lost.

**Verified.** 26 wheel notches and 8 clicks: compositor never restarted, all 11
processes alive, zero new crash records, and the log shows one
`cursor surface ... stays client-side` followed by 8 dropped requests.

---

## 4. Fixed: menus opened but nothing could be selected

Three separate bugs stacked, found only by instrumenting focus events.

**a. Popups had no role.** Firefox creates a real `xdg_popup` for the hamburger
menu. The adapter gave it nothing, so it never mapped and its bare surface
became a card. Popups are now mapped to **`wl_subsurface` of the parent**
(`wl_subcompositor` *is* advertised by this compositor), positioned by
resolving the `xdg_positioner` anchor, gravity and offset, with
`xdg_popup.configure` and `xdg_surface.configure` synthesised so GTK maps it.
Nested submenus resolve their absolute position through the parent chain.

**b. Subsurfaces still became cards.** Fixed by intercepting
`wl_subcompositor.get_subsurface` and tagging the child with a
`wl_webos_shell_surface` carrying our `appId` and
`_WEBOS_WINDOW_TYPE_SUBSURFACE`, a type no window model claims. This also
removed the nameless card that had flashed at **every** launch since the
project began. `WebOSShellSurface` never calls `setRole`, so there is no
conflict with the subsurface role.

**c. Clicks were delivered twice.** The compositor already delivers real
pointer enter, motion and button events to subsurfaces with correct
surface-local coordinates. The adapter was *also* injecting its own from
`/dev/input`, which dragged the seat focus onto the wrong surface. Injection
now happens only when no real pointer event has arrived for 10 seconds. The
wheel is still injected, because the compositor genuinely does not forward it
as a scroll, but it no longer moves the focus.

**d. The actual killer: keyboard focus.** LSM treats a popup subsurface as
separately focusable and hands it the keyboard on click. GTK reads its toplevel
losing focus as a signal to dismiss any open menu, and destroyed the popup in
the 13 ms between press and release, so no item could ever activate. The
adapter now **swallows the popup's keyboard enter and the toplevel's leave
while a menu is open**, so GTK believes it still has focus. Pointer events are
untouched.

**Verified.** Press, release arrives, menu stays open 5 seconds, second click
accepted, menu closes normally.

---

## 5. Fixed: the on-screen keyboard could not be closed

`show_keyboard()` existed; `text_model_hide_input_panel` and `deactivate` were
never called anywhere, and `keyboard_up` was written but never read. There was
no dismissal path in the code at all. Added `hide_keyboard()` with three
triggers: the Back key read straight from evdev (works while the IME panel
holds Wayland focus), a click outside the address-bar strip, and the Wayland
Back path. The IME panel is a separate app (`com.webos.app.keyboard`) and
`applicationManager/close` is denied on the public bus, so the client must ask
via `text_model`.

---

## 6. Fixed: preferences had never been read

`app/distribution/preferences/00-webos.js` was never loaded. Firefox scans
`defaults/pref/*.js` and `defaults/preferences/*.js`; `distribution/` is only
ever `distribution.ini`. **Every preference in that file had been inert for the
life of the project**, including the autoplay policy, the telemetry opt-outs,
and settings forcing software rendering.

The file now lives at `app/defaults/pref/00-webos.js`. When it started being
read, the forced-software-rendering lines it contained became a real
regression, so they were deleted rather than flipped: graphics and video
decoding stay on Firefox's own defaults, which is what the app had always
actually run on.

Two preferences cannot live there, because `browser/omni.ja` sets them later in
the load order. `extensions.webextensions.remote` is one, so `geckotv.c` writes
`profile/user.js` on first run.

---

## 7. Video playback

### The fix that mattered: window size

There is no GPU path, so every pixel is rasterised, composited and copied
through shared memory on the CPU. The adapter was claiming a 1920x1080 window.
It now claims **1280x720** and lets the compositor scale to the panel.

| | 1080p | 720p |
|---|---|---|
| Dropped frames, YouTube | 25 to 50% | under 1% |
| Parent process CPU | 168% | 83% |

The dropped-frame figures come from the browser and are exact. The CPU figures
were taken with an earlier sampler that stretched its window (see section 10),
so treat them as a before-and-after ratio rather than absolute values.

Control test, a plain MP4 over 160 seconds: **16 dropped frames out of 4,965**,
0.3%, no errors. Playback is smooth. Override with `WEBOS_XDG_SIZE=1920x1080`
in the launcher's `env` file.

### Codecs

Firefox's bundled `libmozavcodec` carries only the royalty-free codecs, and
there is no 64-bit system FFmpeg to load. `about:support` originally read
`H264 NONE, AAC NONE, VP9 SWDEC, AV1 NONE`. `build/ffmpeg-mini.sh` (since removed with the 64-bit build) built a
2.7 MB FFmpeg with just H.264, AAC and MP3 and dropped it into the runtime;
`about:support` now reads `H264 SWDEC | VP9 SWDEC | AAC SWDEC | MP3 SWDEC`.

**Never set `media.mediasource.webm.enabled=false`.** Before FFmpeg was
shipped, that steered YouTube to MP4 with AAC audio that could not be decoded
at all. AV1 stays disabled because there is no decoder for it.

### Memory

Measure with `scripts/ffmem.sh`, which uses PSS from `/proc/PID/smaps_rollup`.
Summing RSS overstates by roughly 100 MB because shared pages are counted once
per process.

| Stage | Firefox | Free RAM | Swap |
|---|---|---|---|
| Before this work | 660 MB | 40 MB | 599/599, full |
| Idle, after | 382 MB | 201 MB | none of Firefox swapped |

What helped: capping the Media Source video buffer at 50 MB (Firefox's default
is **300 MB**, larger than the TV's spare memory and the original cause of
mid-video failures), one content process, no Fission, no prelaunched process,
ceilings on caches and the JS heap, and no remote extension process.

**What broke things:** disabling `media.rdd-process`, `media.utility-process`
and `media.utility-ffmpeg` saved about 30 MB and removed every decoder, giving
"your browser can't play this video". Those processes *are* the decoders.

`scripts/tv-zram.sh` puts 512 MB of lz4-compressed swap in RAM ahead of the
flash partition, which stops page-ins being flash reads. Runtime only; copy it
to `/var/lib/webosbrew/init.d/65-geckotv-zram` to persist.

---

## 8. Fixed: YouTube stopped after 30 to 80 seconds

Playback begins, buffers healthily, then YouTube's own player reports
**`onError` code 5** and shows "Something went wrong".

This is not a Firefox media failure. Ruled out by direct test: the codec (both
VP9 and H.264), the buffer caps (fails with Firefox's 300 MB defaults too),
quality switching (fails with quality pinned), dropped frames (fails at under
1%), the JS heap cap, audio, and WebGL. `MediaSource:5` and `MediaDecoder:5`
logs show frames playing in sync with no exceptions right up to the moment
YouTube calls `Detach`. **A plain MP4 plays indefinitely in the same browser.**

YouTube's stats report server-side adaptive streaming. Buffering correctly and
then refusing to continue is the shape of a client-attestation check this build
cannot satisfy. Nothing on our side is likely to change it.

---

**Newer evidence (2026-09-22, 32-bit build, GPU rendering).** Hooks on
`SourceBuffer`, `MediaSource`, `fetch` and the `<video>` element show the
media side is healthy: 0-3 dropped frames, a 60 s buffer, no append
exceptions and no `MediaError`. The SABR `videoplayback` POSTs keep returning
HTTP 200 after about 50 s but carry no media: appends stop while the requests
go on. The player plays out its buffer and raises `onError 5` with 14-20 s
still buffered, then empties the element. YouTube's server is withholding
video, the pattern for a client it scores as a bot. Every launch until then
passed `--marionette`, which makes `navigator.webdriver` true on every page.
The launcher now enables Marionette only when `<app dir>/marionette` exists.
Without it the same video played uninterrupted for 3 minutes, until the TV
was switched off, and the user reports YouTube working. **Never leave
Marionette on for normal use.**

## 9. GPU rendering through the 64-bit bridge: a dead end

Solved instead by the native 32-bit build; see section 13.

`org.webosbrew.bridge-64to32` ships a 64-bit EGL and GLES that forward to the
TV's 32-bit driver, and its libEGL does export the EGL 1.4 core (versioned
`@@EGL_1_4`, which a naive `nm | comm` diff misses). Two obstacles:

- Its libEGL references GLES symbols without declaring `libGLESv2` as a
  dependency, so `dlopen` fails unless GLES is already global. `geckotv.c` has
  an opt-in `gl-bridge` marker that sets `LD_PRELOAD` accordingly.
- More fundamentally, the bridge's design has a **32-bit `gles_proxy` process
  own the Wayland window**. Firefox's window is its own, so there is nothing
  for the bridge to draw into. This needs changes to the bridge, not to us.

The 64-bit build reports `WebRender (Software)` and `HW_COMPOSITING:
blocked by platform`.

---

## 10. Resource footprint

Measured with `scripts/ffmem.sh` for memory and `scripts/ffcpu.sh` for CPU, while the user watched YouTube normally. The window was 1280x720.
The zram swap from `scripts/tv-zram.sh` was active, giving 1111 MB of swap.

### Memory, as PSS

| Situation | Firefox total | Largest process |
|---|---|---|
| Fresh start, idle | about 385 MB | parent 285 MB |
| Plain MP4 playing | about 575 MB | parent 330 MB |
| YouTube playing | 810 to 860 MB | YouTube tab 440 to 495 MB |

The YouTube tab keeps growing. It went from 315 MB to 495 MB in one session,
including while it sat on the error screen. With YouTube open, about 290 MB
stayed available, and swap rose from 740 MB to 820 MB within one minute. So
YouTube's page, rather than Firefox or video decoding, is what fills memory.

### CPU, as a share of the whole TV (4 cores)

| Situation | Firefox | Compositor | Rest of webOS | TV total |
|---|---|---|---|---|
| Idle, or on YouTube's error screen | about 1% | about 1% | about 33% | about 41% |
| YouTube playing | 25 to 42% | about 10% | 23 to 37% | 78 to 94% |

About a third of the processor is always busy with webOS's own services.
During YouTube playback the TV has little spare capacity left.

### Disk on the TV

| Item | Size |
|---|---|
| Firefox runtime, including the FFmpeg libraries | 318.5 MB |
| Profile | 303 MB |
| Debug logs and push backups from development | 100 MB |
| **Total** | **713.5 MB** |

The profile breaks down as follows. The page cache (`cache2`) is 137 MB, and
Firefox's smart sizing lets it grow toward 1 GB. The security and blocklist
data (`remote-settings`) is 59 MB. Site storage is 36 MB. Firefox Suggest
(`suggest.sqlite`) is 17 MB, and the startup cache is 16 MB. The cache lives in
the profile because `geckotv.c` points `XDG_CACHE_HOME` there.

Two savings are available but not yet applied. Deleting the development logs
(`geckotv.log.*`, `media.log.*`) and the `.bak-*` files that
`scripts/push-fix.sh` leaves behind saves about 100 MB. Capping
`browser.cache.disk.capacity` at around 64 MB saves about 70 MB and stops the
cache from growing. Neither change affects how the browser behaves.

The install package (0.1.3) is 110.5 MB and expands to about 324 MB installed. The
FFmpeg libraries add 2.7 MB. The runtime is dominated by `libxul.so` at
159 MB and the two `omni.ja` archives at 92 MB. Everything is already stripped
and bundles only one locale. Only a rebuild with features removed would shrink
the runtime. `build/linux-build.sh` carries those flags for the next build,
and the expected saving is 20 to 30 MB.

---

## 11. Diagnostic notes and traps

**Driving the browser.** The launcher passes `-marionette` and
`-remote-allow-system-access`. From a workstation:
`ssh -f -N -L 2828:127.0.0.1:2828 root@TV`, then a small Marionette client can
navigate and, in chrome context, read and write live prefs via `Services.prefs`
and call `Troubleshoot.snapshot()`. One session at a time. Preferences set this
way persist into `prefs.js`; clear them with `clearUserPref` when done.

**YouTube's own player** is scriptable on the watch page:
`document.getElementById('movie_player')` exposes `getStatsForNerds()`,
`getPlayerState()`, `setPlaybackQualityRange()` and `addEventListener('onError')`.

**Firefox logging.** Put `KEY=VALUE` lines in `<app dir>/env` for one run, e.g.
`MOZ_LOG=MediaSource:5,MediaDecoder:5` with `MOZ_LOG_FILE`. Delete the file
afterwards. `<app dir>/wayland-debug` turns on `WAYLAND_DEBUG=1`, which is how
the popup path was found.

**Input events on this TV are 16 bytes, not 24.** Userspace is 32-bit, so
`struct input_event` is `tv_sec(4) tv_usec(4) type(2) code(2) value(4)`.
Decoding a raw `/dev/input` capture at 24 bytes yields garbage that still looks
partly plausible. Divide the byte count by 16 to sanity-check.

**Two process-detection traps cost real time.** busybox `ps` truncates the
command line, so `ps | grep firefox-runtime/firefox` under-detects; and a
`/proc` scan over full command lines matches the scanning shell's own
arguments, reporting a phantom process. Test `argv[0]` only and skip `$$`.
`scripts/push-fix.sh` does this correctly.

**Sample CPU in one pass.** A shell loop that runs `awk` on each
`/proc/PID/stat` takes several seconds on this busy TV. Each process is then
read at a different moment, so the window becomes about 15 seconds instead of
10, and the per-process figures come out roughly 1.5 times too high. They can
add up to more than 400 percent on a four-core machine. The fix is to read
every file with one `awk "{print \$1, \$14+\$15}" /proc/[0-9]*/stat`, map
PIDs to programs afterwards, and check the total against the `cpu` line in
`/proc/stat`. `scripts/ffcpu.sh` does this.

**Marionette drives the live browser.** If someone is using the TV, every
`Navigate` replaces what they are watching. Measure their session passively
instead, using read-only scripts and `/proc`. Only reload a page when it is
already showing an error.

**An errored YouTube page looks idle.** When YouTube shows its error
screen, Firefox drops to about 1% CPU. So check the player state before
treating a sample as a playback measurement.

**Removing a pref from `user.js` does not unset it.** Firefox copies
`user.js` values into `prefs.js` when it exits. To drop one, stop Firefox and
delete the line from both files.

**busybox `awk` has no `strtonum`.** Pull capture files to a workstation and
decode them there.

**`luna-send` from a root shell** returns empty for most query methods, and
`applicationManager/close` is denied on the public bus. `launch` works.

**When the remote seems dead**, check `dmesg | grep MR25GB` before blaming the
app. A Magic Remote whose Bluetooth link is flapping keeps its buttons working
while the pointer and wheel go silent, which looks exactly like a software bug.

---

## 12. A 32-bit softfp Rust build works on the TV

Tested 2026-09-22 with `experiments/rust-softfp`, built five ways and run on
the TV. This answers whether Firefox's Rust could target the TV's own 32-bit
userspace instead of going through the 64-bit bridge.

**Rust already supports softfp.** The official `armv7-linux-androideabi`
target is softfp (`llvm-floatabi: soft` with `+vfp3d16`). Only 32-bit glibc
Linux lacks a ready-made softfp target: stock `armv7-unknown-linux-gnueabi`
is `+soft-float`. Two ways fill the gap:

- Stable Rust on the stock target with
  `RUSTFLAGS="-C target-feature=-soft-float,+vfp3,+neon"`. rustc warns that
  these features are unstable but builds. The standard library stays
  prebuilt soft-float; your own code uses the FPU.
- A custom target, `armv7-webos-linux-gnueabi.json`, derived from the stock
  one with only the features changed, on nightly with
  `-Zbuild-std=std,panic_abort -Zjson-target-spec`. Everything, including
  the standard library, uses the FPU and NEON.

**Correctness, all 32-bit glibc builds:** results from the TV's own `libm`
(`pow`, `atan2`, `expf`, `fmaf`) match exactly, and an offscreen render
through the TV's EGL and GLES reports `Mali-G52`, `OpenGL ES 3.2`, and reads
back `glClearColor(0.25, 0.5, 0.75, 1)` as `[64, 128, 191, 255]`. Floats
therefore cross into the TV's 32-bit GPU driver correctly. The binaries need
glibc 2.34 or 2.35, which the TV has.

**Speed, best of two rounds, ms (TV load average ~18 during the runs):**

| Build | nbody | saxpy | 20M float calls | 2M libm sin |
|---|---|---|---|---|
| 32-bit hardfp (static) | 78 | 126 | 248 | 86 |
| 32-bit softfp, custom target | 87 | 130 | 246 | 77 |
| 32-bit softfp, stable + features | 89 | 130 | 250 | 80 |
| 64-bit (static) | 88 | 134 | 223 | 73 |
| 32-bit stock soft-float | 4,774 | 2,238 | 2,701 | 67 |

softfp, hardfp and 64-bit are within run-to-run noise of each other. The
calling-convention cost of softfp does not show up even in a loop of 20 million
float-argument calls. Stock soft-float is 20 to 50 times slower. A Mandelbrot
benchmark was excluded: the compiler still optimised it away in some builds.

**Conclusion:** the premise that forced the 64-bit bridge does not hold. A
32-bit Firefox's Rust code can run at full floating-point speed and link
against the TV's own libraries, including its GPU driver. What remains is the
rest of a 32-bit build: a cross toolchain with glibc 2.35-compatible headers,
32-bit builds of Firefox's other dependencies, and Firefox's build system
accepting a custom or feature-modified Rust target.

**Hardware decoding is not a V4L2 question.** The `/dev/video*` devices and
the `v4l2_live` kernel driver belong to live input. LG's decoders sit behind
its own media stack, so hardware video in Firefox would need a decoder module
for that stack, which is only reachable from a 32-bit build.

---

## 13. Native 32-bit Firefox: builds and runs on the TV

Built 2026-09-22 with `build/sysroot-armel.sh`, `build/linux-build-arm32.sh`
and `build/assemble-runtime-arm32.sh`, following the section 12 experiment.
Compile time was about 25 minutes on 12 cores.

**Toolchain.** clang 19 targeting `arm-linux-gnueabi` with
`-march=armv7-a -mthumb -mfpu=neon -mfloat-abi=softfp`; Rust
`armv7-unknown-linux-gnueabi` with `-soft-float,+vfp3,+neon`. The sysroot is
Debian 11 armel (glibc 2.31) with GCC 11's C++ headers from Debian 12 and the
TV's own `libstdc++.so.6.0.29` for linking.

**Obstacles found and fixed, each as a patch or a build setting:**

1. *Firefox chose a hard-float Rust target* (`thumbv7neon-...-gnueabihf`)
   for any non-hard float ABI: with an empty suffix every target "ends with"
   it. `build/patches/rust-target-softfp.patch` excludes hf targets unless the
   ABI is hard and prefers the exact environment.
2. *Debian 11's GCC 10 C++ headers are too old*: `std::lerp` with mixed float
   and double arguments is ambiguous there. GCC 11 headers fix it and match
   the TV's runtime exactly.
3. *The TV's libstdc++ references glibc 2.32 to 2.35 symbols* the 2.31
   sysroot lacks. Linking uses `--allow-shlib-undefined`, carried on `CC` and
   `CXX` because configure's link tests ignore `LDFLAGS`. Firefox's own code
   is still resolved against 2.31.
4. *Debian sysroot symlinks were absolute* and pointed at the build
   machine's `/lib`; `build/sysroot-relink.py` rewrites them.
5. *libjpeg-turbo's NEON assembly* declared `.fpu neon` before
   `.arch armv7a`; LLVM's assembler resets FPU extensions on `.arch`, giving
   1,308 errors. `build/patches/libjpeg-neon-arch-order.patch` swaps them.
6. *The TV's Mali driver needs `GLIBCXX_3.4.29`*, newer than Debian 11's, so
   the runtime must not bundle libstdc++. It takes glibc, libstdc++,
   libgcc_s, EGL, GLES, wayland-egl, wayland-server, gbm, drm, PulseAudio and
   ALSA from the TV and bundles the rest (GTK stack, X11 libs, libffi.so.7).

**Result.**

| | 64-bit via bridge | 32-bit native |
|---|---|---|
| Runtime size | 324 MB | 253 MB |
| `libxul.so` | 159 MB | 123 MB |
| Package (`mach package`) | | 65.7 MB tar.xz |
| Newest glibc symbol | 2.38 (bridge's 2.39) | 2.30 (TV has 2.35) |
| Newest libstdc++ symbol | bundled | 3.4.29 (the TV's own) |
| Memory, headless on the same Wikipedia page | 539 MB | **440 MB (-18%)** |

`libxul` contains 117,000 hardware floating-point and 171,000 NEON
instructions and no calls to software float helpers. Its ELF attributes say
`Tag_CPU_arch: v5TE`, inherited from Debian's ARMv5 support objects; the code
itself is ARMv7.

On the TV, `firefox --version` prints `Mozilla Firefox 153.3.0esr`, every
library resolves, and headless screenshots of example.com and a Wikipedia
article render correctly. The adapter loads in its 32-bit form.

**On screen (2026-09-22).** The window maps, takes remote input, and plays
media through the TV's own FFmpeg 5.0 (`libavcodec.so.59`: H.264, VP9, AV1,
HEVC, AAC and MP3 software decoders), so the bundled FFmpeg is not needed.
It needed two runtime fixes: the adapter library must be installed mode 0755
(a 0600 copy is unreadable to the jail user, and the loader silently falls
back to the TV's libwayland: "No supported shell interface"), and GTK needs
bundled gdk-pixbuf loaders with a cache (`build/gdk-pixbuf-loaders.cache`).
Never `patchelf` these binaries: setting an rpath made every one segfault.

**GPU rendering works (2026-09-22).** Firefox reports `WebRender` (not
Software), with hardware and OpenGL compositing available, on the Mali-G52.
Four problems stood in the way, found in this order:

1. *glxtest failed* because Mali has no `eglQueryDeviceStringEXT`.
   `build/patches/glxtest-optional-device-query.patch` makes that query
   optional; the probe then reports ARM Mali-G52, GLES 3.2.
2. *The adapter freed virtual proxies too early.* The 32-bit runtime's GTK
   comes from Debian 11, whose protocol code was generated by wayland-scanner
   1.18: it sends a destroy request with `wl_proxy_marshal()` and then calls
   `wl_proxy_destroy()`. (The 64-bit GTK uses one `wl_proxy_marshal_flags()`
   call with `WL_MARSHAL_FLAG_DESTROY`.) The adapter freed its virtual
   `xdg_positioner` on the request, so GTK's `wl_proxy_destroy()` touched
   freed memory. This crashed **every menu open**, in software mode too.
   Now the adapter frees on the request only when the destroy flag is set;
   otherwise our libwayland routes `wl_proxy_destroy()` of an id-0 proxy
   (only virtual proxies have id 0) to `webos_xdg_virtual_destroyed()`.
3. *The adapter adopted Mali's private objects.* Every `wl_display`
   request replaced the adapter's display pointer, including requests through
   the short-lived wrapper Mali's EGL makes for its own queue, and every new
   registry replaced `g.registry`, including Mali's. The adapter now keeps the
   real display (`webos_xdg_proxy_display()`) and the first registry on the
   default queue (`webos_xdg_on_default_queue()`).
4. *Mali and libwayland 1.22 disagree about destroyed queues.* Mali's EGL,
   built against an older libwayland, destroys a private queue while its
   `mali_buffer_sharing` proxy is still attached ("queue destroyed while
   proxies still attached"), then wraps that proxy on every surface. Older
   libwayland left a dangling queue pointer, which wrapping only copied.
   1.22 sets it to NULL and adds wrappers to the queue's proxy list, so
   `wl_proxy_create_wrapper()` crashed at address `0xc`. (The TV's own
   libwayland is `libwayland-client.so.0.20.0`, i.e. 1.20, which is what
   Mali was built against.) Our libwayland now
   gives such a wrapper the default queue and logs it (at most 12 times:
   "wrapping mali_buffer_sharing@N whose queue was destroyed"). This is
   expected on every launch.

The "Failed to create EGLContext!: 0x300c" line still appears once at
startup: it comes from a first probe on the display that ends without a
context. The next attempt succeeds: KHR robustness gets `EGL_BAD_ATTRIBUTE`,
then EXT robustness creates the context. It is harmless.

**Measured.** Scrolling a Wikipedia article continuously from inside the
page for 10 s at 1280x720, sampled with `scripts/ffcpu.sh`:

| | Software WebRender | GPU WebRender (2 runs) |
|---|---|---|
| Firefox, % of one core | 125% | 108%, 105% |
| Compositor | 47% | 23%, 23% |
| Whole TV, % of 4 cores | 82% | 67%, 68% |
| Animation frames delivered | 88 per second | 125, 128 per second |

The GPU path gives 40% more frames for less CPU. That is about 40% less
Firefox CPU per frame, and the compositor's share halves because it no
longer copies shared-memory frames. Memory with the page loaded is about the
same: 365 MB (GPU) against 376 MB (software), PSS. Mali's buffers live in CMA
and are not counted there.

**How the crashes were found.** glibc's `backtrace()` prints nothing on this
ARM build and `libxul` has no symbol table, so the adapter's crash handler
now prints pc, lr and the code addresses found on the stack as
`library+offset` (`build/symbolize-arm32.py` resolves them against an
unstripped library). Two preloadable tracers are in `experiments/`:
`printf-trace` records the caller and format of the last printf-family call,
which the crash handler prints, and `egl-trace` is a `libEGL.so.1` that logs
display, config, context and surface calls with attributes and errors.
Firefox opens `libEGL.so` before `libEGL.so.1`, so the tracer needs both
names.

**Not started:** hardware video decoding through LG's media stack, which
needs a custom Firefox decoder module and handling for video shown on a
separate display plane.

---

## 14. webOS 4: what is known

No webOS 4 TV is available. Two sources so far:

**LG's webOS TV 4.0 emulator** (`Emulator_tv_linux_v4.0.0.zip` from the
Internet Archive, release `4.0.0-15209 (goldilocks-gayasan)`). It is an x86
build, so it only gives library versions, not ARM behaviour. Its disk has a
small unencrypted boot system (glibc **2.24**, libstdc++ **6.0.22**, i.e.
GCC 6, `GLIBCXX_3.4.22`), and the real system in a LUKS-encrypted partition,
which was not opened. LG's bundled Open Source Software Notice lists the
rest: Wayland **1.11.0**, FFmpeg **2.3.6** (`libavcodec.so.55`), GLib 2.48.2,
Cairo 1.14.6, HarfBuzz 1.2.7, FreeType 2.6.5, fontconfig 2.12.1, libffi
3.2.1, libxkbcommon 0.6.1, ICU 57.1, NSS 3.23, Qt 5.6.2, Linux 4.8.

**A user's log from a webOS 4 TV** running the old 64-bit build: the TV has
a 64-bit kernel; the adapter's registry handling and compositor version clamp
(`wl_compositor` v3, `wl_output` v2) worked; GDK then found **no `wl_seat`**
and Firefox crashed in `gdk_seat_get_keyboard` on a NULL seat.

**What that means for a webOS 4 build:**

- glibc 2.24 is below this build's floor (2.30, from the Debian 11 sysroot).
  The sysroot has to move to Debian 9 (glibc 2.24), and the GTK stack and
  other bundled libraries with it.
- libstdc++ 6.0.22 is far older than Firefox needs, so libstdc++ is linked
  statically (see `build/mozconfig-arm32`).
- The adapter must cope with a compositor that offers no seat at startup.
- Firefox still tries `libavcodec.so.55`, so the TV's FFmpeg 2.3 may load;
  which decoders LG built in is unknown.
- The TV's libwayland is 1.11, older than Mali-era 1.20 on webOS 25.

---

## 15. Portable build against buildroot-nc4 (glibc 2.12)

Built 2026-09-24 so one package covers webOS 4 onwards. The nc4 SDK
(openlgtv/buildroot-nc4, commit `322ff04e`) targets every webOS TV: ARMv7
NEON softfp, GCC 16.2, glibc 2.12.2 with a small `glibc-polyfills` library.

**Checked first, on the webOS 25 TV:** a Rust program (threads, TLS, files,
hashing, time) and a C++20 program, both built against nc4, ran correctly.
Rust's std needs `getauxval` (glibc 2.16), which the polyfills provide.
clang 19 accepts GCC 16.2's C++ headers. nc4's `libstdc++.a` is built for
ARMv7 against glibc 2.12, so it needs neither the `__sync_*` helpers nor the
`__libc_single_threaded` stand-in the Debian build needs.

**The SDK** (`build/nc4/build-sdk.sh`, `build/nc4/firefox.fragment`) adds
GTK 3 (Wayland only), Pango, Cairo, HarfBuzz, GDK-Pixbuf, at-spi2-core and
libdrm to nc4's `webos_tv_defconfig`. Changes needed:

- Kernel headers 3.17 (`BR2_DEFAULT_KERNEL_VERSION`; the `..._CUSTOM_3_17`
  option alone only declares the minimum). GTK's Wayland backend needs
  `memfd_create`. Binaries then need a 3.17+ kernel.
- Wayland 1.18.0 and wayland-protocols 1.20 instead of nc4's 1.11 pins (GTK
  needs 1.14.91 and 1.17). Both are the last autotools releases, which nc4's
  package files use; checksums match upstream Buildroot. The TVs are not
  affected: Firefox bundles its own libwayland-client, the adapter.
- Serial top-level build, as nc4 does: with per-package directories nc4's
  `glibc-polyfills` finds no compiler.

**Firefox against it** (`build/mozconfig-nc4`) needed:

- `build/patches/allow-static-libstdcxx.patch`: configure refuses a static
  libstdc++. The Debian build only passed that check because its test
  program failed to link.
- `build/nc4/glibc-compat.h`, force-included: `MADV_*`, `O_PATH`,
  `AT_EMPTY_PATH`, `AT_NO_AUTOMOUNT` constants; `secure_getenv` mapped to
  `__secure_getenv` (exported by glibc 2.12, and still by 2.35 as
  `@GLIBC_2.4`); a `putenv` declaration for NSS. The header must include no
  system header (it comes before each file's `_GNU_SOURCE`) and must give
  glibc functions default visibility (Firefox hides symbols by default).
- `-lrt -ldl` (glibc 2.12 keeps these apart) and `-lglibc_polyfills`.
- The adapter calls `process_vm_readv` through `syscall()` (the glibc
  wrapper is 2.15).

**Result:** all 57 files of the runtime need glibc 2.12 or older
(`build/nc4/glibc-check.sh`), none needs the TV's libstdc++. Runtime 248 MB,
package 0.1.5 103 MB (Debian-based 0.1.4: 107 MB). On the webOS 25 TV: GPU
WebRender, all codecs, Wikipedia, menus, and 3.5 minutes of YouTube without
Marionette. Untested on a webOS 4 TV; there the missing `wl_seat` (section
14) is still expected to stop it.

---

## 16. Experiment: Firefox compiled by GCC instead of clang (does not work)

Goal: one toolchain, with nc4's GCC 16.2 and GNU ld compiling and linking
Firefox's C and C++ (`build/mozconfig-nc4-gcc`). Rust stays with rustc, and
bindgen needs libclang whatever compiles the C++.

**Build problems, both solved:**

- bindgen's libclang found the container's own cross GCC 13 headers instead
  of nc4's GCC 16 (`bits/c++config.h` not found), which broke the Rust
  bindings. Fixed with `BINDGEN_CFLAGS=--gcc-install-dir=<nc4 GCC 16>`.
  (With clang as the compiler this came for free through `--gcc-install-dir`
  in `CC`.)
- `libmozinference.so` (llama.cpp) failed to link on `__extendhfsf2`: with
  `-mfpu=neon` GCC 16 calls that half-float helper, which its ARM libgcc only
  has as `__gnu_h2f_ieee`. nc4 builds everything with `neon-fp16`, whose
  conversion instructions all NEON webOS CPUs have; using it fixes the link.

**Result:** all binaries need glibc 2.12 or older and no shared libstdc++,
but Firefox crashes (SIGSEGV) two seconds after start in
`Servo_GetComputedKeyframeValues`, Rust style code called from GCC-compiled
C++. Rust reads C++ structures through bindgen-generated layouts, computed
with clang; a crash there points to GCC laying out at least one structure
differently on 32-bit ARM. Mozilla tests GCC builds only on x86-64. Finding
the mismatch would take layout checks across several full rebuilds.

**Size, even if fixed:** `libxul.so` 143.3 MB against clang's 129.5 MB. Code
(`.text`) is 95.5 MB against 86.5 MB (+10%), and GNU ld cannot pack
relocations on 32-bit ARM (3.8 MB `.rel.dyn` against LLD's 0.1 MB
`.relr.dyn`), all of which are processed at every start. The package was
75 MB against 56 MB for the Firefox tarball.

**Decision:** Firefox stays with clang 19 and LLD. Everything else in the nc4
build comes from nc4's GCC 16.2: the SDK's libraries, the C++ library linked
into Firefox, the adapter (`CROSS=nc4`) and the launcher (Makefile `TC=`).
Enabling GCC's `-Wl,--exclude-libs` hiding and static libstdc++ worked the
same way as with clang.

---

## 17. What the nc4 runtime takes from the TV, and what it bundles

Audited on package 0.1.7 (unpacked), for webOS 4 in particular.

**Taken from the TV** (all old enough for webOS 4):

| Library | Newest version needed | webOS 4 |
|---|---|---|
| glibc (`libc`, `libm`, `libpthread`, `libdl`, `librt`, `libresolv`, `ld-linux.so.3`) | GLIBC_2.12 | 2.24 |
| `libgcc_s.so.1` | GCC_4.3.0 | GCC 6 |
| `libasound.so.2` | ALSA_0.9 | alsa-lib 1.1.2 |

Opened at run time, all optional or present: `libEGL`/`libGLESv2` (GPU),
`libpulse.so.0`, `libavcodec.so.53` to `.63` (webOS 4 has `.55`),
`libgbm`/`libdrm` (DMA-BUF), `libudev`. No file carries `DT_RELR` (Firefox's
relrhack handles LLD's packed relocations), and the executables' ELF ABI tag
is kernel 3.17.

**Bundled because the TV's copy may be missing or wrong for our GLib/GTK:**

- `fallback/libwayland-egl.so.1`: added to the library path by the launcher
  only if the TV has none (webOS 4); webOS 25 keeps its own, which its Mali
  driver is built against.
- `glib-schemas/gschemas.compiled` (`GSETTINGS_SCHEMA_DIR`, with
  `GSETTINGS_BACKEND=memory`): GTK aborts when a schema it asks for is
  missing, e.g. `org.gtk.Settings.FileChooser` when a page opens a file
  picker.
- `gio-modules/`, empty (`GIO_MODULE_DIR`): otherwise GLib 2.88 would load
  the TV's GIO plug-ins, built for its own GLib (2.48 on webOS 4).
- `xkb/` from xkeyboard-config 2.38, added to the nc4 SDK
  (`XKB_CONFIG_ROOT`): xkbcommon 1.9 rejects LG's keymap ("Keycode too big",
  keycodes above 0xfff) and builds a default keymap from this data instead.
- `gtk-immodules/` (`GTK_IM_MODULE_FILE`): GTK's Wayland input-method module
  (section on the on-screen keyboard).

`build/nc4/glibc-check.sh` checks every file against glibc 2.12;
`build/assemble-runtime.sh` runs it on each nc4 runtime. `smoke` (the GLES
test client run when no runtime is installed) is the only file linked
against `libEGL`, `libGLESv2` and `libwayland-webos-client` directly.


---

## 18. webOS 4 on LG's emulator: compositor, fixes, what works

Tested 2026-09-24 on LG's webOS TV 4.0 emulator in QEMU (build/emu/README.md)
with a 32-bit x86 build of Firefox and the same adapter code as the TV build.

**The compositor** offers what a user's webOS 4 TV log showed: `wl_compositor`
v3, `wl_shm` v1, `wl_output` v2, `wl_seat` v2, `wl_shell`,
`wl_webos_shell`, `text_model_factory`, `wl_webos_surface_group_compositor`,
and **no `wl_subcompositor`, no `wl_data_device_manager`**. It also has
LSM's surface groups. Its UI runs at 1280x720 scaled to 1080p.

**Why 0.1.9 crashed there:** GTK sets up no seat without
`wl_data_device_manager` (`gdk_seat_get_keyboard` assertion), and Firefox
asserts on a missing `wl_subcompositor` in `nsWaylandDisplay::Init`.

**The adapter now (only when those globals are missing):**
- offers a do-nothing `wl_data_device_manager` (clipboard stays inside
  Firefox) and a stand-in `wl_subcompositor`;
- shows Firefox's content surface, a subsurface of the main window, as a
  layer of a surface group rooted at the main window. webOS 4 only displays a
  group member that has a `wl_shell` toplevel role; a bare surface stays
  invisible even as a group member. Group membership keeps it from being
  fullscreened as a separate card (`getForegroundAppInfo` lists the window as
  group owner plus member);
- never destroys layers or groups: webOS 4's versions of those interfaces
  lack requests the XML has, so `destroy` has another opcode there and
  sending opcode 2 was a fatal protocol error ("invalid method 2"). Layers are
  reused; a surface keeps its `wl_shell` role while it lives, because asking
  for a second one is also fatal;
- binds `wl_seat` at no more than the compositor's version (v2 there).

**Works in the emulator:** start, page rendering, clicks (pointer events
arrive on the layer), typing through the hardware keyboard, and the webOS
keyboard opening for the address bar.

**Crash one second in on a real webOS 4 TV (0.1.10), not in the emulator:**
a Rust panic in `uuid::Uuid::new_v4`, because the `getrandom` crate failed.
glibc 2.24 has no `getrandom()`, so the crate falls back to `/dev/urandom`,
but first opens `/dev/random` to wait for the kernel's pool, and webOS 4's
native-app jail (`/etc/jail_native.conf`: `copynod /dev/urandom` only) has
no `/dev/random`. The emulator build carries glibc 2.36 and ran outside the
jail. Fix (0.1.11): `src/getrandom-compat.c`, a `getrandom()` that makes the
system call (falling back to `/dev/urandom`), which the launcher preloads
when the TV's glibc lacks one; the crate finds it through `dlsym`.

**Menus and pop-ups (0.1.13).** webOS 4's compositor shows no small window
at a place the app picks (its QML, read in the emulator): a window group
stretches every member over the owner window (`anchors.fill`), POPUP windows
are centred or pinned to an edge by location hint, and FLOATING windows are
one at a time at 0,0. So the adapter draws pop-ups itself: one transparent
full-window overlay, a group layer above the page (z 1000), into which the
pixels of each pop-up's shared-memory buffers are copied when it commits
(webOS 4 TVs render in software, "WebRender (Software)"). The adapter tracks
`wl_shm` pools and buffers for this. The overlay's input region is the
pop-up rectangles; pointer events on it go to the pop-up under them,
translated to its coordinates. Learned on the way:

- webOS 4 does not free a layer when its surface is detached: attaching to it
  again is a fatal "Layer already attached". Layers are never reused (new
  name and z each time) and the overlay is never detached.
- Showing the overlay moves the keyboard focus to it and back; GTK must not
  see that (Firefox closes menus when its window loses focus).
- Firefox makes a pop-up's content surface before the pop-up gets its role,
  and GTK shows some pop-ups (tooltips, some panels) as plain subsurfaces of
  the main window at an offset. Subsurfaces are therefore attached as layers
  only on their first commit, when their position is known; offset ones go
  to the overlay.
- Each pop-up has GTK's own surface (window background) and Firefox's content
  inside it: draw the background first, or the content is hidden.
- A group member given a wl_shell role but not attached to the group is a
  stand-alone window to webOS, which then rearranges the screen.
- Pointer entry events carry screen-scale coordinates (the window is
  1280x720, scaled to 1080p); motion events are right. Hit-testing is done
  on motion.

**Keyboard crash (0.1.14).** Opening the webOS keyboard a second time (a
YouTube search box, the address bar) ended in a fatal "invalid object": webOS
4 destroys a text_model when it is deactivated (a `wl_display.delete_id`
follows the `deactivate`), and the adapter reused it. The protocol has no
destroy request; on webOS 4 the adapter now forgets the model after
deactivating it and makes a new one for the next keyboard. webOS 25 keeps
the model, as before.

**Not yet:** in-page `<select>` lists and other pop-ups are untested on a real
webOS 4 TV. The GPU path (Mali, `wl_mali`) cannot be tested in the
emulator; for TVs without `libwayland-egl.so.1` the runtime now ships
`src/wayland-egl-shim.c`, which uses the GPU driver's own `wl_egl_window`
functions when the driver has them (older Mali drivers do, with their own
struct) and the generic ones otherwise.

## 19. webOS 6: the window is composed in the adapter ("flat mode")

Reported on a 2021 NANO796PC (webOS 6.5.3): the launch screen stays grey
for a minute or two, then the app closes. Firefox itself runs (five
processes as the app's user); webOS never shows its window.

That TV's compositor offers no `wl_subcompositor` and no
`wl_data_device_manager`, like webOS 4, so the adapter took the webOS 4
path: Firefox's content as a surface-group layer. webOS 6 does not draw
those layers. In LG's 6.0 emulator (build/emu, `WEBOS=6`) the layer's item
never gets a parent in the scene (`WebOSSurfaceGroupLayer::attach ...
parent=0x0`); the compositor has an `Eos.SurfaceGroup` QML plugin but no
QML uses it. On the TV the window never even became the foreground app.

Flat mode (`flat_mode()` in `src/webos-xdg.c`): where the compositor has no
`wl_subcompositor` but offers `wl_webos_foreign` (webOS 5 and later; webOS 4
does not), the pop-up overlay surface is the window itself. It gets the
window's roles (wl_shell toplevel, appId, full screen); GTK's window,
Firefox's content surface and every pop-up are tagged
`_WEBOS_WINDOW_TYPE_SUBSURFACE` (webOS 6 makes any untagged surface with
content its fullscreen card, which took the window off the screen when a
menu opened) and copied into it, bottom to top, blended source-over.
`WEBOS_XDG_FLAT=0|1` overrides. Details that mattered:

- Frame callbacks: no surface but the window is shown, so every surface's
  callbacks are asked of the window, from the first one on. Firefox's
  content surface asks before it becomes a subsurface; one callback left
  unanswered and Firefox stops drawing (idle in poll, no menus).
- webOS 6 ignores `set_state(fullscreen)` on a surface without content, so
  it is sent again after the window's first frame.
- `text_model` is version 1 everywhere, but webOS 6 orders its requests
  differently (show_input_panel 8, hide_input_panel 9, set_enter_key_type
  12; ours are 11, 12, 6), read from the 6.0 emulator's
  `libwayland-webos-client`. Sent by the webOS 4 numbering they are a fatal
  "invalid arguments" (the keyboard crashed Firefox). libwayland encodes a
  request from the proxy's own interface, so in flat mode the text model is
  created with a webOS 6 table; `WEBOS_XDG_TEXT_MODEL=4|6` overrides.

Tested in the 6.0 emulator with `WEBOS_XDG_AS_WEBOS4=1` (the emulator's
compositor, unlike the TV's, has `wl_subcompositor`): window, menus and
submenus, links, the on-screen keyboard opening and closing. webOS 4 in its
emulator is unchanged (flat mode stays off). Not yet confirmed on a webOS 6
TV. Cost: every frame is copied once more on the CPU (1280x720).

The launcher also writes its log to `profile/geckotv.log` when the app
folder is not writable by the app's user (an app installed as root).

Installed as root (Homebrew Channel, `dev/install`), the app folder is
root's, mode 755, and webOS runs the app as its own user (6795:5000 on that
TV). Two things then broke Firefox, both reproduced in the 6.0 emulator:

- No profile folder could be made in the app folder. The launcher now falls
  back to `$HOME/.geckotv-profile`, then `/tmp` (lost on reboot), and puts its
  log there too; the log's first line names the choice.
- webOS's app manager sets `XDG_CACHE_HOME=/var/cache/xdg`, which is root's.
  The launcher kept inherited XDG folders, so Firefox could not make its
  profile's cache and stopped at "Your Firefox profile cannot be loaded". It
  now replaces an inherited XDG folder the user cannot write in.

The emulator's app manager runs native apps as root without jailer ("jail
off") and kills an app whose window is not up within 10 seconds ("Transition
is timeout"), which a cold Firefox under TCG misses; see build/emu/README.md.
