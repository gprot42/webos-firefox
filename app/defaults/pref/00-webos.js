// TV defaults for the ESR 153 build.
//
// NOTE: until 2026-09-22 this file sat in distribution/preferences, which
// Firefox never reads, so none of it was ever in effect. Everything here is
// therefore new behaviour and is kept deliberately small. Anything that
// merely restates a Firefox default has been dropped, because the app ran
// on those defaults for its whole life and they are known to work.

// Memory. The TV has 2 GB shared with the rest of webOS, and Firefox was
// measured at ~660 MB resident with swap completely full, which is what
// makes YouTube fail part-way through a video.
pref("dom.ipc.processCount", 1);
pref("dom.ipc.processCount.webIsolated", 1);
pref("fission.autostart", false);
pref("browser.sessionhistory.max_total_viewers", 0);
pref("browser.cache.memory.capacity", 32768);
pref("image.mem.surfacecache.max_size_kb", 32768);
pref("webgl.disabled", true);
// Media Source buffers are the largest single allocation on a video site.
// Firefox lets the video buffer grow to 300 MB by default, which is more
// than this TV has spare and is what exhausts swap part-way through a clip.
pref("media.mediasource.eviction_threshold.video", 52428800);
pref("media.mediasource.eviction_threshold.audio", 10485760);
pref("media.memory_cache_max_size", 8192);
pref("media.cache_size", 32768);
// Do not hold a spare content process in reserve; that is a whole process
// of resident memory for a browser that only ever shows one page at a time.
pref("dom.ipc.processPrelaunch.enabled", false);
// DO NOT disable the media helper processes. Turning off rdd-process,
// utility-process and utility-ffmpeg saved about 30 MB and left Firefox
// with no decoder at all, so YouTube reported "your browser can't play
// this video". Modern Firefox decodes in those processes and the
// in-content fallback is not a complete substitute. The memory is the
// price of playing video.
// One content process really means one: no separate privileged process.
pref("browser.tabs.remote.separatePrivilegedContentProcess", false);
pref("browser.tabs.remote.separatePrivilegedMozillaWebContentProcess", false);
// Nothing here uses the accessibility tree, and initialising it costs both
// memory and time.
pref("accessibility.force_disabled", 1);
pref("browser.sessionstore.max_tabs_undo", 0);
pref("extensions.pocket.enabled", false);
// Ceiling on the JavaScript heap, in KB.
pref("javascript.options.mem.max", 262144);
// No extensions are installed, yet Firefox still runs a WebExtensions
// process (18 MB resident plus 13 MB swapped, measured). Run any that do
// appear in-process instead.
pref("extensions.webextensions.remote", false);
// Hand freed pages back to the kernel promptly instead of holding them, so
// a page that shrinks actually relieves the swap pressure.
pref("memory.free_dirty_pages", true);
pref("dom.memory.foreground_content_processes_have_larger_page_cache", false);

// Autoplay. A TV has no comfortable way to click Play, and the browser is
// the only thing on screen, so neither reason Firefox holds media back
// applies. 0 = allow; the default of 1 blocks anything with sound.
pref("media.autoplay.default", 0);
pref("media.autoplay.blocking_policy", 0);
pref("media.autoplay.block-webaudio", false);
pref("media.block-autoplay-until-in-foreground", false);
// Firefox suspends video for a window it believes is hidden. This
// compositor does not report visibility the way Firefox expects, so that
// fires while the video is plainly on screen and reads as a pause.
pref("media.suspend-background-video.enabled", false);
pref("dom.audiochannel.audioCompeting", false);

// Codecs. This build has NO H.264 and NO AAC decoder: Firefox's bundled
// libmozavcodec only carries the royalty-free codecs, and the system ffmpeg
// it would otherwise dlopen does not exist in 64-bit on this TV. Measured
// via about:support: "H264 NONE, AAC NONE, VP9 SWDEC, VP8 SWDEC, AV1 NONE".
// So WebM (VP9 + Opus) must stay enabled; disabling it steered YouTube to
// MP4 streams with AAC audio that could not be decoded, and playback died
// after about a minute with "Something went wrong". AV1 has no decoder
// either, so keep it off so sites never offer it.
pref("media.av1.enabled", false);

// Graphics and video decoding are left at Firefox's own defaults on
// purpose. The earlier file forced software WebRender and disabled
// hardware video decoding, but since it was never read, every build so far
// has run on the defaults. Forcing software rendering on a TV SoC is a
// large regression, so those lines are gone rather than merely flipped.

// Housekeeping.
pref("browser.shell.checkDefaultBrowser", false);
pref("datareporting.policy.dataSubmissionEnabled", false);
pref("toolkit.telemetry.enabled", false);
pref("toolkit.telemetry.unified", false);
pref("browser.crashReports.unsubmittedCheck.enabled", false);
pref("browser.fullscreen.autohide", false);
pref("browser.sessionstore.resume_from_crash", false);
pref("browser.startup.homepage", "about:blank");
pref("startup.homepage_welcome_url", "");
pref("browser.newtabpage.enabled", false);

// Marionette (off unless <app dir>/marionette exists) applies "recommended"
// test preferences: dummy add-on and blocklist servers, Safe Browsing off,
// updates disabled. If Firefox stops without a clean shutdown they stay in
// prefs.js, which broke the add-on search. Never apply them.
pref("remote.prefs.recommended", false);
