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
`H264 NONE, AAC NONE, VP9 SWDEC, AV1 NONE`. `build/ffmpeg-mini.sh` builds a
2.7 MB FFmpeg with just H.264, AAC and MP3 and drops it into the runtime;
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

## 8. Open: YouTube stops after 30 to 80 seconds

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

## 9. Open: GPU rendering

`org.webosbrew.bridge-64to32` ships a 64-bit EGL and GLES that forward to the
TV's 32-bit driver, and its libEGL does export the EGL 1.4 core (versioned
`@@EGL_1_4`, which a naive `nm | comm` diff misses). Two obstacles:

- Its libEGL references GLES symbols without declaring `libGLESv2` as a
  dependency, so `dlopen` fails unless GLES is already global. `geckotv.c` has
  an opt-in `gl-bridge` marker that sets `LD_PRELOAD` accordingly.
- More fundamentally, the bridge's design has a **32-bit `gles_proxy` process
  own the Wayland window**. Firefox's window is its own, so there is nothing
  for the bridge to draw into. This needs changes to the bridge, not to us.

Firefox reports `WebRender (Software)` and `HW_COMPOSITING: blocked by
platform`. If this is ever solved, the ~170% parent CPU at 1080p should
collapse and the 720p compromise becomes unnecessary.

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

