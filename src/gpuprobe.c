/* GPU probe: does EGL/GLES work on this TV the way Firefox would use it?
 *
 * Firefox only reports "blocklisted" when its own GPU probe (glxtest) hangs
 * or fails. This program walks the same path one logged step at a time, with
 * a watchdog per step, so a single run shows where it stops:
 *
 *   window   an xdg toplevel through the webos-xdg adapter, and a subsurface
 *            for GL content, exactly as Firefox makes them (on webOS 4 the
 *            adapter shows the subsurface as a surface-group layer)
 *   EGL      eglGetDisplay (legacy) and eglGetPlatformDisplay (Wayland
 *            platform), each tried in its own child process
 *   GLES     context, strings, a few coloured frames, swap timing
 *
 * libEGL/libGLESv2 are loaded at run time like Firefox does; libwayland-egl
 * is the TV's own, or the runtime's fallback on TVs without one (webOS 4).
 * Run as root on the TV:  <app dir>/gpuprobe   (it logs to stdout)
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <wayland-client.h>

#include "xdg-shell-client-protocol.h"

#define STEP_TIMEOUT 8

static const char *step_name = "start";
static char app_dir[512];

static double now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static void say(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    fflush(stdout);
}

static void on_alarm(int sig)
{
    (void)sig;
    printf("  HUNG: no progress in %d s at step \"%s\"\n", STEP_TIMEOUT, step_name);
    fflush(stdout);
    _exit(3);
}

static void on_crash(int sig)
{
    printf("  CRASHED: signal %d at step \"%s\"\n", sig, step_name);
    fflush(stdout);
    _exit(4);
}

static void step(const char *name)
{
    step_name = name;
    printf("- %s\n", name);
    fflush(stdout);
    alarm(STEP_TIMEOUT);
}

/* ---- EGL / GLES, loaded at run time ---- */
static void *egl_lib, *gles_lib, *wegl_lib;
static PFNEGLGETPROCADDRESSPROC p_eglGetProcAddress;
#define EGLFN(ret, name, args) static ret (*p_##name) args;
EGLFN(EGLDisplay, eglGetDisplay, (EGLNativeDisplayType))
EGLFN(EGLBoolean, eglInitialize, (EGLDisplay, EGLint *, EGLint *))
EGLFN(const char *, eglQueryString, (EGLDisplay, EGLint))
EGLFN(EGLBoolean, eglChooseConfig, (EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *))
EGLFN(EGLBoolean, eglBindAPI, (EGLenum))
EGLFN(EGLContext, eglCreateContext, (EGLDisplay, EGLConfig, EGLContext, const EGLint *))
EGLFN(EGLSurface, eglCreateWindowSurface, (EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint *))
EGLFN(EGLBoolean, eglMakeCurrent, (EGLDisplay, EGLSurface, EGLSurface, EGLContext))
EGLFN(EGLBoolean, eglSwapBuffers, (EGLDisplay, EGLSurface))
EGLFN(EGLBoolean, eglSwapInterval, (EGLDisplay, EGLint))
EGLFN(EGLint, eglGetError, (void))
EGLFN(EGLBoolean, eglTerminate, (EGLDisplay))
static const GLubyte *(*p_glGetString)(GLenum);
static void (*p_glClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
static void (*p_glClear)(GLbitfield);
static void (*p_glFinish)(void);
struct wl_egl_window;
static struct wl_egl_window *(*p_wl_egl_window_create)(struct wl_surface *, int, int);

static void *open_first(const char *what, const char *const *names)
{
    int i;

    for (i = 0; names[i]; i++) {
        void *h = dlopen(names[i], RTLD_NOW | RTLD_GLOBAL);

        if (h) {
            say("  %s: %s", what, names[i]);
            return h;
        }
    }
    say("  %s: NOT FOUND (%s)", what, dlerror());
    return NULL;
}

static int load_libraries(void)
{
    static const char *const egl[] = { "libEGL.so.1", "libEGL.so", NULL };
    static const char *const gles[] = { "libGLESv2.so.2", "libGLESv2.so", NULL };
    char fallback[600];
    const char *wegl[3] = { "libwayland-egl.so.1", NULL, NULL };
    Dl_info info;

    /* As the launcher does: the TV's libwayland-egl, else the runtime's. */
    if (access("/usr/lib/libwayland-egl.so.1", F_OK) != 0 &&
        access("/lib/libwayland-egl.so.1", F_OK) != 0) {
        snprintf(fallback, sizeof fallback, "%s/firefox-runtime/fallback/libwayland-egl.so.1",
                 app_dir);
        wegl[0] = fallback;
    }
    step("load libraries");
    egl_lib = open_first("libEGL", egl);
    gles_lib = open_first("libGLESv2", gles);
    wegl_lib = open_first("libwayland-egl", wegl);
    if (!egl_lib || !gles_lib || !wegl_lib)
        return 0;
#define LOAD(lib, name) \
    do { *(void **)&p_##name = dlsym(lib, #name); \
         if (!p_##name) { say("  missing symbol %s", #name); return 0; } } while (0)
    LOAD(egl_lib, eglGetProcAddress);
    LOAD(egl_lib, eglGetDisplay);
    LOAD(egl_lib, eglInitialize);
    LOAD(egl_lib, eglQueryString);
    LOAD(egl_lib, eglChooseConfig);
    LOAD(egl_lib, eglBindAPI);
    LOAD(egl_lib, eglCreateContext);
    LOAD(egl_lib, eglCreateWindowSurface);
    LOAD(egl_lib, eglMakeCurrent);
    LOAD(egl_lib, eglSwapBuffers);
    LOAD(egl_lib, eglSwapInterval);
    LOAD(egl_lib, eglGetError);
    LOAD(egl_lib, eglTerminate);
    LOAD(gles_lib, glGetString);
    LOAD(gles_lib, glClearColor);
    LOAD(gles_lib, glClear);
    LOAD(gles_lib, glFinish);
    LOAD(wegl_lib, wl_egl_window_create);
    if (dladdr((void *)p_eglGetDisplay, &info) && info.dli_fname)
        say("  libEGL resolves to %s", info.dli_fname);
    return 1;
}

/* ---- Wayland window, the way Firefox makes it ---- */
static struct {
    struct wl_display *display;
    struct wl_compositor *compositor;
    struct wl_subcompositor *subcompositor;
    struct xdg_wm_base *wm;
    struct wl_surface *main, *content;
    struct xdg_surface *xsurface;
    struct xdg_toplevel *toplevel;
    struct wl_subsurface *sub;
    int width, height, configured;
} w;

static void registry_global(void *data, struct wl_registry *r, uint32_t name,
                            const char *iface, uint32_t version)
{
    (void)data;
    if (!strcmp(iface, "wl_compositor"))
        w.compositor = wl_registry_bind(r, name, &wl_compositor_interface,
                                        version < 4 ? version : 4);
    else if (!strcmp(iface, "wl_subcompositor"))
        w.subcompositor = wl_registry_bind(r, name, &wl_subcompositor_interface, 1);
    else if (!strcmp(iface, "xdg_wm_base"))
        w.wm = wl_registry_bind(r, name, &xdg_wm_base_interface, 1);
}

static void registry_remove(void *data, struct wl_registry *r, uint32_t name)
{
    (void)data;
    (void)r;
    (void)name;
}

static const struct wl_registry_listener registry_listener = { registry_global, registry_remove };

static void wm_ping(void *data, struct xdg_wm_base *wm, uint32_t serial)
{
    (void)data;
    xdg_wm_base_pong(wm, serial);
}

static const struct xdg_wm_base_listener wm_listener = { wm_ping };

static void xs_configure(void *data, struct xdg_surface *s, uint32_t serial)
{
    (void)data;
    xdg_surface_ack_configure(s, serial);
    w.configured = 1;
}

static const struct xdg_surface_listener xs_listener = { xs_configure };

static void tl_configure(void *data, struct xdg_toplevel *t, int32_t width, int32_t height,
                         struct wl_array *states)
{
    (void)data;
    (void)t;
    (void)states;
    if (width > 0 && height > 0) {
        w.width = width;
        w.height = height;
    }
}

static void tl_close(void *data, struct xdg_toplevel *t)
{
    (void)data;
    (void)t;
}

static const struct xdg_toplevel_listener tl_listener = { .configure = tl_configure, .close = tl_close };

static int make_window(void)
{
    struct wl_registry *registry;
    int i;

    step("connect to the compositor");
    w.display = wl_display_connect(NULL);
    if (!w.display) {
        say("  FAILED: wl_display_connect (XDG_RUNTIME_DIR=%s)", getenv("XDG_RUNTIME_DIR"));
        return 0;
    }
    registry = wl_display_get_registry(w.display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(w.display);
    say("  compositor %s, subcompositor %s, xdg_wm_base %s", w.compositor ? "yes" : "NO",
        w.subcompositor ? "yes" : "NO", w.wm ? "yes (adapter)" : "NO");
    if (!w.compositor || !w.subcompositor || !w.wm)
        return 0;

    step("map the window");
    xdg_wm_base_add_listener(w.wm, &wm_listener, NULL);
    w.width = 1280;
    w.height = 720;
    w.main = wl_compositor_create_surface(w.compositor);
    w.xsurface = xdg_wm_base_get_xdg_surface(w.wm, w.main);
    xdg_surface_add_listener(w.xsurface, &xs_listener, NULL);
    w.toplevel = xdg_surface_get_toplevel(w.xsurface);
    xdg_toplevel_add_listener(w.toplevel, &tl_listener, NULL);
    xdg_toplevel_set_title(w.toplevel, "GPU probe");
    xdg_toplevel_set_fullscreen(w.toplevel, NULL);
    wl_surface_commit(w.main);
    for (i = 0; i < 50 && !w.configured; i++)
        wl_display_roundtrip(w.display);
    say("  window %dx%d, configured %s", w.width, w.height, w.configured ? "yes" : "NO");

    /* Content in a subsurface, as Firefox does (its MozContainer). */
    w.content = wl_compositor_create_surface(w.compositor);
    w.sub = wl_subcompositor_get_subsurface(w.subcompositor, w.content, w.main);
    wl_subsurface_set_position(w.sub, 0, 0);
    wl_subsurface_set_desync(w.sub);
    wl_display_roundtrip(w.display);
    return 1;
}

static const char *egl_err(void)
{
    static char buf[16];

    snprintf(buf, sizeof buf, "0x%04x", p_eglGetError());
    return buf;
}

/* One EGL variant, in a child process: display, context, frames. */
static int run_variant(int platform)
{
    EGLDisplay dpy;
    EGLConfig cfg;
    EGLContext ctx;
    EGLSurface surf;
    struct wl_egl_window *ew;
    EGLint major = 0, minor = 0, n = 0;
    static const EGLint cfg_attr[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE
    };
    static const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    double t0, t;
    int i;

    if (!load_libraries() || !make_window())
        return 1;

    if (platform) {
        PFNEGLGETPLATFORMDISPLAYEXTPROC gpd;

        step("eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND)");
        gpd = (PFNEGLGETPLATFORMDISPLAYEXTPROC)p_eglGetProcAddress("eglGetPlatformDisplay");
        if (!gpd)
            gpd = (PFNEGLGETPLATFORMDISPLAYEXTPROC)p_eglGetProcAddress("eglGetPlatformDisplayEXT");
        if (!gpd) {
            say("  not available (no eglGetPlatformDisplay[EXT])");
            return 1;
        }
        dpy = gpd(EGL_PLATFORM_WAYLAND_KHR, w.display, NULL);
    } else {
        step("eglGetDisplay(wl_display)");
        dpy = p_eglGetDisplay((EGLNativeDisplayType)w.display);
    }
    if (dpy == EGL_NO_DISPLAY) {
        say("  FAILED: no display (%s)", egl_err());
        return 1;
    }
    say("  OK");

    step("eglInitialize");
    if (!p_eglInitialize(dpy, &major, &minor)) {
        say("  FAILED: %s", egl_err());
        return 1;
    }
    say("  OK: EGL %d.%d", major, minor);
    say("  vendor:     %s", p_eglQueryString(dpy, EGL_VENDOR));
    say("  version:    %s", p_eglQueryString(dpy, EGL_VERSION));
    say("  client API: %s", p_eglQueryString(dpy, EGL_CLIENT_APIS));
    say("  extensions: %s", p_eglQueryString(dpy, EGL_EXTENSIONS));

    step("eglChooseConfig (RGBA8888, GLES2, window)");
    if (!p_eglChooseConfig(dpy, cfg_attr, &cfg, 1, &n) || n < 1) {
        say("  FAILED: %d configs (%s)", n, egl_err());
        return 1;
    }
    say("  OK");

    step("eglCreateContext (GLES 2)");
    p_eglBindAPI(EGL_OPENGL_ES_API);
    ctx = p_eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    if (ctx == EGL_NO_CONTEXT) {
        say("  FAILED: %s", egl_err());
        return 1;
    }
    say("  OK");

    step("wl_egl_window_create + eglCreateWindowSurface");
    ew = p_wl_egl_window_create(w.content, w.width, w.height);
    if (!ew) {
        say("  FAILED: wl_egl_window_create returned NULL");
        return 1;
    }
    surf = p_eglCreateWindowSurface(dpy, cfg, (EGLNativeWindowType)ew, NULL);
    if (surf == EGL_NO_SURFACE) {
        say("  FAILED: %s", egl_err());
        return 1;
    }
    say("  OK");

    step("eglMakeCurrent");
    if (!p_eglMakeCurrent(dpy, surf, surf, ctx)) {
        say("  FAILED: %s", egl_err());
        return 1;
    }
    say("  OK");
    say("  GL vendor:   %s", (const char *)p_glGetString(GL_VENDOR));
    say("  GL renderer: %s", (const char *)p_glGetString(GL_RENDERER));
    say("  GL version:  %s", (const char *)p_glGetString(GL_VERSION));
    say("  GLSL:        %s", (const char *)p_glGetString(GL_SHADING_LANGUAGE_VERSION));

    /* Firefox renders with swap interval 0 and paces itself. */
    step("render 180 frames (red, green, blue for about a second each)");
    p_eglSwapInterval(dpy, 0);
    t0 = now_ms();
    for (i = 0; i < 180; i++) {
        float c = (float)(i % 60) / 60.0f;

        alarm(STEP_TIMEOUT);
        if (i < 60)
            p_glClearColor(0.4f + 0.6f * c, 0.0f, 0.0f, 1.0f);
        else if (i < 120)
            p_glClearColor(0.0f, 0.4f + 0.6f * c, 0.0f, 1.0f);
        else
            p_glClearColor(0.0f, 0.0f, 0.4f + 0.6f * c, 1.0f);
        p_glClear(GL_COLOR_BUFFER_BIT);
        if (!p_eglSwapBuffers(dpy, surf)) {
            say("  FAILED at frame %d: eglSwapBuffers %s", i, egl_err());
            return 1;
        }
        wl_display_dispatch_pending(w.display);
        wl_display_flush(w.display);
        usleep(16000);
    }
    p_glFinish();
    t = now_ms() - t0;
    say("  OK: 180 frames in %.0f ms (%.1f fps with a 16 ms pause per frame)", t, 180000.0 / t);
    say("  (on screen: the window turned red, then green, then blue)");

    step("clean up");
    p_eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    p_eglTerminate(dpy);
    alarm(0);
    say("  OK");
    return 0;
}

static int child(int platform)
{
    pid_t pid;
    int status;

    say("");
    say("=== %s ===", platform ? "EGL on the Wayland platform (eglGetPlatformDisplay)"
                             : "EGL, legacy display (eglGetDisplay)");
    pid = fork();
    if (pid == 0) {
        signal(SIGALRM, on_alarm);
        signal(SIGSEGV, on_crash);
        signal(SIGBUS, on_crash);
        signal(SIGABRT, on_crash);
        signal(SIGILL, on_crash);
        _exit(run_variant(platform));
    }
    if (pid < 0 || waitpid(pid, &status, 0) < 0)
        return -1;
    if (WIFEXITED(status)) {
        switch (WEXITSTATUS(status)) {
        case 0: say("RESULT: works"); break;
        case 3: say("RESULT: hangs"); break;
        case 4: say("RESULT: crashes"); break;
        default: say("RESULT: fails"); break;
        }
        return WEXITSTATUS(status);
    }
    say("RESULT: killed by signal %d", WTERMSIG(status));
    return -1;
}

int main(void)
{
    struct utsname u;
    char exe[512];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    int a, b;

    if (n > 0) {
        char *slash;

        exe[n] = '\0';
        slash = strrchr(exe, '/');
        if (slash)
            *slash = '\0';
        snprintf(app_dir, sizeof app_dir, "%s", exe);
    }
    /* The launcher's environment. */
    setenv("XDG_RUNTIME_DIR", "/tmp/xdg", 0);
    setenv("WAYLAND_DISPLAY", "wayland-0", 0);
    setenv("APPID", "com.github.gprot42.geckotv", 0);

    uname(&u);
    say("GPU probe for Firefox on webOS (%s %s %s)", u.sysname, u.release, u.machine);
    a = child(0);
    b = child(1);
    say("");
    say("SUMMARY: legacy display %s, platform display %s",
        a == 0 ? "works" : a == 3 ? "hangs" : a == 4 ? "crashes" : "fails",
        b == 0 ? "works" : b == 3 ? "hangs" : b == 4 ? "crashes" : "fails");
    return a == 0 || b == 0 ? 0 : 1;
}
