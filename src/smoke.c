#define _GNU_SOURCE

/* Wayland smoke client for the OLED55C56LB (webOS 10, softfp).
 *
 * RetroArch's webOS backend is the sequence that actually maps a native
 * window on this compositor: wl_shell toplevel, then wl_webos_shell with
 * appId, then fullscreen. xdg_wm_base is present in surface-manager, and
 * this client logs it, but that role is not what LSM uses for an app
 * surface. A surface can take only one shell role.
 */

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <wayland-client.h>
#include <wayland-egl.h>
#include <wayland-webos-shell-client-protocol.h>

#define APP_ID "com.github.gprot42.geckotv"

static struct {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shell *shell;
    struct wl_shell_surface *shell_surface;
    struct wl_webos_shell *webos_shell;
    struct wl_webos_shell_surface *webos_surface;
    struct wl_output *output;
    struct wl_surface *surface;
    struct wl_egl_window *egl_window;
    EGLDisplay egl_display;
    EGLContext egl_context;
    EGLSurface egl_surface;
    int width;
    int height;
    int saw_xdg;
    volatile sig_atomic_t running;
    FILE *logf;
} g;

static void log_msg(const char *fmt, ...)
{
    char line[512];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    fprintf(stderr, "%s\n", line);
    fflush(stderr);
    if (g.logf) {
        fprintf(g.logf, "%s\n", line);
        fflush(g.logf);
    }
}

static void open_log(void)
{
    char exe[PATH_MAX];
    char path[PATH_MAX];
    ssize_t n;
    char *slash;

    n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n < 0 || (size_t)n >= sizeof exe - 16)
        return;
    exe[n] = '\0';
    slash = strrchr(exe, '/');
    if (!slash)
        return;
    *slash = '\0';
    {
        size_t len = strlen(exe);
        if (len + sizeof "/smoke.log" > sizeof path)
            return;
        memcpy(path, exe, len);
        memcpy(path + len, "/smoke.log", sizeof "/smoke.log");
    }
    g.logf = fopen(path, "a");
}

static void die(const char *msg)
{
    log_msg("fatal: %s", msg);
    exit(1);
}

static void on_signal(int sig)
{
    (void)sig;
    g.running = 0;
}

static const char *app_id(void)
{
    const char *id = getenv("APPID");

    if (!id || !id[0] || strcmp(id, "com.palm.devmode.openssh") == 0)
        return APP_ID;
    return id;
}

static void shell_ping(void *data, struct wl_shell_surface *surface, uint32_t serial)
{
    (void)data;
    wl_shell_surface_pong(surface, serial);
}

static void shell_configure(void *data, struct wl_shell_surface *surface,
                            uint32_t edges, int32_t width, int32_t height)
{
    (void)data;
    (void)surface;
    (void)edges;
    log_msg("wl_shell configure %dx%d", width, height);
    if (width > 0 && height > 0) {
        g.width = width;
        g.height = height;
        if (g.egl_window)
            wl_egl_window_resize(g.egl_window, width, height, 0, 0);
    }
}

static void shell_popup_done(void *data, struct wl_shell_surface *surface)
{
    (void)data;
    (void)surface;
}

static const struct wl_shell_surface_listener shell_listener = {
    shell_ping,
    shell_configure,
    shell_popup_done,
};

static void webos_state_changed(void *data, struct wl_webos_shell_surface *surface, uint32_t state)
{
    (void)data;
    (void)surface;
    log_msg("webos state %u", state);
}

static void webos_position_changed(void *data, struct wl_webos_shell_surface *surface,
                                   int32_t x, int32_t y)
{
    (void)data;
    (void)surface;
    log_msg("webos position %d,%d", x, y);
}

static void webos_close(void *data, struct wl_webos_shell_surface *surface)
{
    (void)data;
    (void)surface;
    log_msg("webos close");
    g.running = 0;
}

static void webos_exposed(void *data, struct wl_webos_shell_surface *surface, struct wl_array *rectangles)
{
    (void)data;
    (void)surface;
    (void)rectangles;
    log_msg("webos exposed");
}

static void webos_state_about_to_change(void *data, struct wl_webos_shell_surface *surface, uint32_t state)
{
    (void)data;
    (void)surface;
    log_msg("webos state about to change %u", state);
}

static void webos_addon_status(void *data, struct wl_webos_shell_surface *surface, uint32_t status)
{
    (void)data;
    (void)surface;
    (void)status;
}

static const struct wl_webos_shell_surface_listener webos_listener = {
    webos_state_changed,
    webos_position_changed,
    webos_close,
    webos_exposed,
    webos_state_about_to_change,
    webos_addon_status,
};

static void output_geometry(void *data, struct wl_output *output, int32_t x, int32_t y,
                            int32_t phys_w, int32_t phys_h, int32_t subpixel,
                            const char *make, const char *model, int32_t transform)
{
    (void)data;
    (void)output;
    (void)x;
    (void)y;
    (void)phys_w;
    (void)phys_h;
    (void)subpixel;
    (void)transform;
    log_msg("output %s %s", make ? make : "?", model ? model : "?");
}

static void output_mode(void *data, struct wl_output *output, uint32_t flags,
                        int32_t width, int32_t height, int32_t refresh)
{
    (void)data;
    (void)output;
    log_msg("output mode %dx%d refresh %d flags %#x", width, height, refresh, flags);
    if ((flags & WL_OUTPUT_MODE_CURRENT) && width > 0 && height > 0) {
        g.width = width;
        g.height = height;
    }
}

static void output_done(void *data, struct wl_output *output)
{
    (void)data;
    (void)output;
}

static void output_scale(void *data, struct wl_output *output, int32_t factor)
{
    (void)data;
    (void)output;
    log_msg("output scale %d", factor);
}

static const struct wl_output_listener output_listener = {
    output_geometry,
    output_mode,
    output_done,
    output_scale,
};

static void registry_global(void *data, struct wl_registry *registry, uint32_t id,
                            const char *interface, uint32_t version)
{
    (void)data;
    log_msg("global %s v%u id %u", interface, version, id);
    if (strcmp(interface, "wl_compositor") == 0) {
        g.compositor = wl_registry_bind(registry, id, &wl_compositor_interface,
                                        version < 3 ? version : 3);
    } else if (strcmp(interface, "wl_shell") == 0) {
        g.shell = wl_registry_bind(registry, id, &wl_shell_interface, 1);
    } else if (strcmp(interface, "wl_webos_shell") == 0) {
        g.webos_shell = wl_registry_bind(registry, id, &wl_webos_shell_interface, 1);
    } else if (strcmp(interface, "wl_output") == 0 && !g.output) {
        uint32_t bind_version = version < 2 ? version : 2;
        g.output = wl_registry_bind(registry, id, &wl_output_interface, bind_version);
        wl_output_add_listener(g.output, &output_listener, NULL);
    } else if (strcmp(interface, "xdg_wm_base") == 0) {
        g.saw_xdg = 1;
    }
}

static void registry_remove(void *data, struct wl_registry *registry, uint32_t id)
{
    (void)data;
    (void)registry;
    (void)id;
}

static const struct wl_registry_listener registry_listener = {
    registry_global,
    registry_remove,
};

static void init_egl(void)
{
    EGLint major = 0;
    EGLint minor = 0;
    EGLint nconfig = 0;
    EGLConfig config;
    EGLint config_attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_NONE
    };
    EGLint context_attribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display;

    get_platform_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)
        eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (get_platform_display)
        g.egl_display = get_platform_display(EGL_PLATFORM_WAYLAND_KHR, g.display, NULL);
    if (g.egl_display == EGL_NO_DISPLAY)
        g.egl_display = eglGetDisplay((EGLNativeDisplayType)g.display);
    if (g.egl_display == EGL_NO_DISPLAY)
        die("eglGetDisplay");
    if (!eglInitialize(g.egl_display, &major, &minor))
        die("eglInitialize");
    log_msg("EGL %d.%d vendor %s", major, minor, eglQueryString(g.egl_display, EGL_VENDOR));
    if (!eglChooseConfig(g.egl_display, config_attribs, &config, 1, &nconfig) || nconfig < 1) {
        /* Mali on this TV sometimes rejects an alpha buffer. */
        config_attribs[7] = 0;
        if (!eglChooseConfig(g.egl_display, config_attribs, &config, 1, &nconfig) || nconfig < 1)
            die("eglChooseConfig");
    }
    if (!eglBindAPI(EGL_OPENGL_ES_API))
        die("eglBindAPI");
    g.egl_context = eglCreateContext(g.egl_display, config, EGL_NO_CONTEXT, context_attribs);
    if (g.egl_context == EGL_NO_CONTEXT)
        die("eglCreateContext");
    g.egl_window = wl_egl_window_create(g.surface, g.width, g.height);
    if (!g.egl_window)
        die("wl_egl_window_create");
    g.egl_surface = eglCreateWindowSurface(g.egl_display, config, (EGLNativeWindowType)g.egl_window, NULL);
    if (g.egl_surface == EGL_NO_SURFACE)
        die("eglCreateWindowSurface");
    if (!eglMakeCurrent(g.egl_display, g.egl_surface, g.egl_surface, g.egl_context))
        die("eglMakeCurrent");
    eglSwapInterval(g.egl_display, 1);
    log_msg("GLES %s, renderer %s", glGetString(GL_VERSION), glGetString(GL_RENDERER));
}

static int dispatch_events(int timeout_ms)
{
    struct pollfd pfd;
    int ret;

    while (wl_display_prepare_read(g.display) != 0)
        wl_display_dispatch_pending(g.display);
    if (wl_display_flush(g.display) < 0 && errno != EAGAIN) {
        wl_display_cancel_read(g.display);
        return -1;
    }
    pfd.fd = wl_display_get_fd(g.display);
    pfd.events = POLLIN;
    ret = poll(&pfd, 1, timeout_ms);
    if (ret > 0)
        wl_display_read_events(g.display);
    else
        wl_display_cancel_read(g.display);
    wl_display_dispatch_pending(g.display);
    return 0;
}

int main(int argc, char **argv)
{
    const char *display_id;
    struct wl_region *region;
    unsigned frames = 0;

    (void)argc;
    (void)argv;
    open_log();
    log_msg("smoke start APPID=%s WAYLAND_DISPLAY=%s XDG_RUNTIME_DIR=%s",
            getenv("APPID") ? getenv("APPID") : "(unset)",
            getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") : "(unset)",
            getenv("XDG_RUNTIME_DIR") ? getenv("XDG_RUNTIME_DIR") : "(unset)");

    g.running = 1;
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    g.display = wl_display_connect(NULL);
    if (!g.display)
        die("wl_display_connect failed");
    g.registry = wl_display_get_registry(g.display);
    wl_registry_add_listener(g.registry, &registry_listener, NULL);
    wl_display_roundtrip(g.display);

    if (!g.compositor || !g.shell || !g.webos_shell)
        die("compositor did not offer wl_compositor, wl_shell, and wl_webos_shell");
    log_msg("xdg_wm_base %s", g.saw_xdg ? "advertised, unused" : "not advertised");

    g.surface = wl_compositor_create_surface(g.compositor);
    g.shell_surface = wl_shell_get_shell_surface(g.shell, g.surface);
    if (!g.shell_surface)
        die("wl_shell_get_shell_surface");
    wl_shell_surface_add_listener(g.shell_surface, &shell_listener, NULL);
    wl_shell_surface_set_toplevel(g.shell_surface);

    g.webos_surface = wl_webos_shell_get_shell_surface(g.webos_shell, g.surface);
    if (!g.webos_surface)
        die("wl_webos_shell_get_shell_surface");
    wl_webos_shell_surface_add_listener(g.webos_surface, &webos_listener, NULL);

    display_id = getenv("DISPLAY_ID");
    if (!display_id || !display_id[0])
        display_id = "0";
    wl_webos_shell_surface_set_property(g.webos_surface, "appId", app_id());
    wl_webos_shell_surface_set_property(g.webos_surface, "title", "Firefox");
    wl_webos_shell_surface_set_property(g.webos_surface, "displayAffinity", display_id);
    wl_webos_shell_surface_set_property(g.webos_surface, "_WEBOS_ACCESS_POLICY_KEYS_BACK", "true");
    wl_webos_shell_surface_set_property(g.webos_surface, "_WEBOS_ACCESS_POLICY_KEYS_EXIT", "true");
    wl_surface_commit(g.surface);
    wl_display_roundtrip(g.display);

    wl_webos_shell_surface_set_state(g.webos_surface, WL_WEBOS_SHELL_SURFACE_STATE_FULLSCREEN);
    wl_surface_commit(g.surface);
    wl_display_roundtrip(g.display);

    if (g.width <= 0 || g.height <= 0) {
        g.width = 1920;
        g.height = 1080;
        log_msg("no output mode yet, using %dx%d", g.width, g.height);
    }
    log_msg("window %dx%d", g.width, g.height);

    region = wl_compositor_create_region(g.compositor);
    wl_region_add(region, 0, 0, g.width, g.height);
    wl_surface_set_opaque_region(g.surface, region);
    wl_region_destroy(region);

    init_egl();

    while (g.running) {
        float phase = (frames % 240) / 240.0f;

        if (dispatch_events(16) < 0)
            break;
        glViewport(0, 0, g.width, g.height);
        glClearColor(0.05f, 0.25f + 0.45f * phase, 0.55f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        if (!eglSwapBuffers(g.egl_display, g.egl_surface)) {
            log_msg("eglSwapBuffers failed");
            break;
        }
        if (frames == 0 || frames % 300 == 0)
            log_msg("frame %u", frames);
        frames++;
    }

    log_msg("smoke exit after %u frames", frames);
    if (g.logf)
        fclose(g.logf);
    return 0;
}
