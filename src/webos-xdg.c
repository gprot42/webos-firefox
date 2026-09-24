#define _GNU_SOURCE

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <pthread.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-util.h>

#include "xdg-shell-client-protocol.h"
#include "wayland-webos-shell-client-protocol.h"
#include "webos-input-manager-client-protocol.h"
#include "text-model-client-protocol.h"
#include "text-input-unstable-v3-client-protocol.h"

/* Compiled into libwayland-client. Requests are intercepted after
 * libwayland has parsed the argument list, then translated onto the
 * wl_shell and wl_webos_shell globals this compositor actually exports.
 */

#define MAX_VIRT 48

extern struct wl_proxy *webos_xdg_create_virtual(struct wl_proxy *factory,
                                                  const struct wl_interface *interface,
                                                  uint32_t version);
extern void webos_xdg_destroy_virtual(struct wl_proxy *proxy);
void webos_xdg_virtual_destroyed(struct wl_proxy *proxy);
void webos_xdg_null_queue_report(struct wl_proxy *proxy, void *caller);
struct wl_display *webos_xdg_proxy_display(struct wl_proxy *proxy);
int webos_xdg_on_default_queue(struct wl_proxy *proxy);

struct peek {
    const struct wl_interface *interface;
};

enum virt_kind {
    V_WM = 1,
    V_POSITIONER,
    V_SURFACE,
    V_TOPLEVEL,
    V_POPUP,
    V_TEXT_INPUT_MANAGER,
    V_TEXT_INPUT
};

struct virt {
    struct wl_proxy *proxy;
    enum virt_kind kind;
    const void *listener;
    void *data;
    struct wl_proxy *wl_surface;
    struct wl_shell_surface *shell_surface;
    struct wl_webos_shell_surface *webos_surface;
    struct wl_subsurface *subsurface;
    uint32_t version;
    int injected;
    int configured;
    /* xdg_positioner state, and the resolved popup geometry. */
    int32_t p_w, p_h;
    int32_t p_ax, p_ay, p_aw, p_ah;
    uint32_t p_anchor, p_gravity;
    int32_t p_ox, p_oy;
    int32_t popup_x, popup_y;
    int32_t abs_x, abs_y;
};

static struct {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_shell *shell;
    struct wl_webos_shell *webos;
    struct wl_webos_input_manager *input;
    uint32_t xdg_name;
    struct virt virts[MAX_VIRT];
    uint32_t serial;
} g;

static uint32_t host_shell_name;
static uint32_t host_webos_name;
static uint32_t host_input_name;
static uint32_t host_text_name;
static uint32_t host_seat_name;
static uint32_t host_shm_name;
static uint32_t host_compositor_name;
static uint32_t host_subcompositor_name;
static struct wl_subcompositor *our_subcompositor;
/* Surfaces GDK names in wl_pointer.set_cursor. LSM maps any surface that
 * gets content as a card (FullscreenView.checkFullscreen checks neither
 * role nor appId) and minimizes the browser window for it, so these
 * surfaces must never carry content. */
#define MAX_CURSOR 8
static struct wl_proxy *cursor_surfaces[MAX_CURSOR];

/* LSM makes a WebOSSurfaceItem for every wl_surface that gets content, and
 * FullscreenView fullscreens any of them whose type is still the default
 * _WEBOS_WINDOW_TYPE_CARD, which minimizes the browser. Subsurfaces are
 * ours and Firefox's own (MozContainer), and none of them should be a card,
 * so give each one a type no window model claims. The surface keeps being
 * composited inside its parent. */
#define MAX_TAGGED 32
static struct wl_proxy *tagged_surfaces[MAX_TAGGED];

#define MAX_HOOK 24

struct hook {
    struct wl_proxy *proxy;
    const void *listener;
    void *data;
};

static struct hook pointer_hooks[MAX_HOOK];
static struct hook keyboard_hooks[MAX_HOOK];
static struct hook *focused_keys;
/* The surface our synthetic pointer events are currently inside. GDK drops
 * motion and button unless the seat has a focus, and that focus only moves
 * on enter/leave, so we have to send those ourselves. */
static struct wl_surface *synth_focus;
/* The compositor does deliver real pointer enter/motion/button to our
 * surfaces, including subsurfaces, with correct surface-local coordinates.
 * Injecting our own from evdev on top of that delivered every click twice
 * and dragged the seat focus onto the wrong surface, which is why menu
 * items never activated. Inject only while no real pointer is arriving. */
static struct timespec last_real_pointer;

static int real_pointer_active(void)
{
    struct timespec now;
    long ms;

    if (last_real_pointer.tv_sec == 0)
        return 0;
    clock_gettime(CLOCK_MONOTONIC, &now);
    ms = (now.tv_sec - last_real_pointer.tv_sec) * 1000
       + (now.tv_nsec - last_real_pointer.tv_nsec) / 1000000;
    return ms < 10000;
}

static void note_real_pointer(void)
{
    clock_gettime(CLOCK_MONOTONIC, &last_real_pointer);
}
static struct wl_seat *our_seat;
static struct wl_surface *main_surface;
static struct text_model *text_input;
static int saw_motion;
static int last_x;
static int last_y;
static int keyboard_up;

#define PTR_QUEUE 64
struct ptr_ev {
    int button;
    int down;
    int x;
    int y;
};
static struct ptr_ev ptr_q[PTR_QUEUE];
static int ptr_q_head;
static int ptr_q_tail;
static pthread_mutex_t ptr_mu = PTHREAD_MUTEX_INITIALIZER;
static struct wl_display *wake_display;
static int wake_pending_flag;

static void show_keyboard(void);
static void hide_keyboard(void);
static void text_input_enter_main(void);
static void ensure_text_model(void);
static void start_remote_pointer(void);
static uint32_t server_compositor_version;
static uint32_t server_output_version;
static int emitting;
/* Size the browser is told it has. LSM scales a fullscreen surface to the
 * panel, so a 1280x720 window costs 2.25x less to rasterise, composite
 * and copy through shared memory than 1920x1080, at the price of a softer
 * picture. Measured on YouTube 720p: dropped frames fell from ~25% to
 * under 1% and the parent process from ~170% to ~80% CPU. Override with
 * WEBOS_XDG_SIZE=WxH in the launcher's env file, e.g. 1920x1080. */
static int win_w = 1280;
static int win_h = 720;

static void read_window_size(void)
{
    const char *s = getenv("WEBOS_XDG_SIZE");
    int w, h;

    if (s && sscanf(s, "%dx%d", &w, &h) == 2 && w >= 640 && h >= 360 && w <= 3840 && h <= 2160) {
        win_w = w;
        win_h = h;
    }
}
static volatile int mapped_one;

__attribute__((format(printf, 1, 2)))
static void log_msg(const char *fmt, ...)
{
    va_list ap;
    struct timespec ts;
    struct tm tm;

    /* UTC, so lines can be lined up against /var/log/messages. */
    clock_gettime(CLOCK_REALTIME, &ts);
    gmtime_r(&ts.tv_sec, &tm);
    fprintf(stderr, "%02d:%02d:%02d.%03ld webos-xdg: ",
            tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    fflush(stderr);
}

/* Reads our own memory without faulting: fails with EFAULT on unmapped or
 * reserved pages. Called through syscall() because glibc only added the
 * process_vm_readv() wrapper in 2.15 and older webOS has 2.12. */
static ssize_t read_own_memory(void *dst, const void *src, size_t n)
{
    struct iovec local = { dst, n };
    struct iovec remote = { (void *)src, n };

    return syscall(SYS_process_vm_readv, getpid(), &local, 1UL, &remote, 1UL, 0UL);
}

/* Executable mappings, read once in the crash handler without stdio or
 * malloc (the fault may be inside malloc). */
static char crash_maps[192 * 1024];
static struct { unsigned long lo, hi, pgoff; const char *name; } crash_exec[512];
static int crash_nexec;
static unsigned long crash_stack_hi;

static void crash_load_maps(unsigned long sp)
{
    int fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    size_t len = 0;
    ssize_t r;
    char *p, *end;

    if (fd < 0)
        return;
    while (len < sizeof crash_maps - 1 &&
           (r = read(fd, crash_maps + len, sizeof crash_maps - 1 - len)) > 0)
        len += r;
    close(fd);
    crash_maps[len] = '\0';
    for (p = crash_maps; *p && crash_nexec < 512; p = end + 1) {
        unsigned long lo, hi, pgoff;
        char *q, *slash;

        end = strchr(p, '\n');
        if (!end)
            break;
        *end = '\0';
        lo = strtoul(p, &q, 16);
        hi = strtoul(q + 1, &q, 16);
        if (sp >= lo && sp < hi)
            crash_stack_hi = hi;
        if (q[3] != 'x')                       /* " r-xp" */
            continue;
        pgoff = strtoul(q + 6, &q, 16);
        slash = strrchr(q, '/');
        crash_exec[crash_nexec].lo = lo;
        crash_exec[crash_nexec].hi = hi;
        crash_exec[crash_nexec].pgoff = pgoff;
        crash_exec[crash_nexec].name = slash ? slash + 1 : "[anon]";
        crash_nexec++;
    }
}

static int crash_in_code(unsigned long a)
{
    int i;

    for (i = 0; i < crash_nexec; i++)
        if (a >= crash_exec[i].lo && a < crash_exec[i].hi)
            return 1;
    return 0;
}

static int crash_in_libc(unsigned long a)
{
    int i;

    for (i = 0; i < crash_nexec; i++)
        if (a >= crash_exec[i].lo && a < crash_exec[i].hi)
            return !strcmp(crash_exec[i].name, "libc.so.6");
    return 0;
}

/* Prints "what 0x... lib+0xoff"; returns 1 if the address is in code. */
static int crash_resolve(const char *what, unsigned long a)
{
    int i;

    for (i = 0; i < crash_nexec; i++)
        if (a >= crash_exec[i].lo && a < crash_exec[i].hi) {
            dprintf(STDERR_FILENO, "webos-xdg:   %s %#lx %s+%#lx\n", what, a,
                    crash_exec[i].name, a - crash_exec[i].lo + crash_exec[i].pgoff);
            return 1;
        }
    dprintf(STDERR_FILENO, "webos-xdg:   %s %#lx\n", what, a);
    return 0;
}

/* From experiments/printf-trace when it is preloaded; NULL otherwise. */
struct printf_trace {
    void *caller;
    const char *fmt;
    const char *fn;
};
extern struct printf_trace *webos_printf_trace_get(void) __attribute__((weak));

static void on_crash(int sig, siginfo_t *info, void *ctx)
{
    ucontext_t *uc = ctx;
    unsigned long pc = 0, lr = 0, sp = 0;
    int i, found = 0;
    struct printf_trace last = { 0 };

    /* Copy before our own dprintf calls overwrite it. */
    if (webos_printf_trace_get)
        last = *webos_printf_trace_get();

    dprintf(STDERR_FILENO, "\nwebos-xdg: signal %d addr=%p thread %ld\n", sig,
            info ? info->si_addr : NULL, (long)syscall(SYS_gettid));
#if defined(__arm__)
    pc = uc->uc_mcontext.arm_pc;
    lr = uc->uc_mcontext.arm_lr;
    sp = uc->uc_mcontext.arm_sp;
#elif defined(__aarch64__)
    pc = uc->uc_mcontext.pc;
    lr = uc->uc_mcontext.regs[30];
    sp = uc->uc_mcontext.sp;
#endif
    crash_load_maps(sp);
    crash_resolve("pc", pc);
    crash_resolve("lr", lr);
    if (webos_printf_trace_get) {
        struct printf_trace *t = &last;
        char fmt[160] = "";
        size_t len = sizeof fmt - 1;

        /* The format may sit at the end of a mapping: fall back to less. */
        while (t->fmt && len > 8 && read_own_memory(fmt, t->fmt, len) < 0)
            len /= 2;
        fmt[t->fmt ? len : 0] = '\0';
        for (i = 0; fmt[i]; i++)
            if (fmt[i] == '\n')
                fmt[i] = '|';
        dprintf(STDERR_FILENO, "webos-xdg:   last %s format \"%s\"\n", t->fn ? t->fn : "?", fmt);
        crash_resolve("called from", (unsigned long)t->caller);
    }
    /* No unwind tables to trust here, so list stack words that point into
     * code outside libc: the real return addresses are among them, in call
     * order, mixed with stale values. */
    for (i = 0; sp && i < 16384 && found < 32; i++) {
        unsigned long *w = (unsigned long *)sp + i;

        if ((unsigned long)(w + 1) > crash_stack_hi)
            break;
        if (crash_in_code(*w) && !crash_in_libc(*w))
            found += crash_resolve("stack", *w);
    }
    _exit(128 + sig);
}

__attribute__((constructor)) static void webos_xdg_init(void)
{
    struct sigaction sa;

    setvbuf(stderr, NULL, _IONBF, 0);
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_crash;
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGTRAP, &sa, NULL);
    read_window_size();
    log_msg("ready, window %dx%d", win_w, win_h);
    start_remote_pointer();
}

static const struct wl_interface *proxy_iface(struct wl_proxy *proxy)
{
    return ((struct peek *)proxy)->interface;
}

static struct virt *virt_get(struct wl_proxy *proxy)
{
    int i;
    if (!proxy)
        return NULL;
    for (i = 0; i < MAX_VIRT; i++) {
        if (g.virts[i].proxy == proxy)
            return &g.virts[i];
    }
    return NULL;
}

static struct virt *virt_add(struct wl_proxy *proxy, enum virt_kind kind)
{
    int i;
    for (i = 0; i < MAX_VIRT; i++) {
        if (!g.virts[i].proxy) {
            memset(&g.virts[i], 0, sizeof g.virts[i]);
            g.virts[i].proxy = proxy;
            g.virts[i].kind = kind;
            return &g.virts[i];
        }
    }
    log_msg("virt table full");
    return NULL;
}

static struct wl_proxy *new_virt(struct wl_proxy *factory, const struct wl_interface *iface,
                                 uint32_t version, enum virt_kind kind, struct virt **out)
{
    struct wl_proxy *created = webos_xdg_create_virtual(factory, iface, version);
    struct virt *child;

    if (out)
        *out = NULL;
    if (!created) {
        log_msg("virtual proxy failed");
        return NULL;
    }
    if (!version)
        version = 1;
    child = virt_add(created, kind);
    if (child)
        child->version = version;
    if (out)
        *out = child;
    return created;
}

static void shell_ping(void *data, struct wl_shell_surface *surface, uint32_t serial)
{
    (void)data;
    wl_shell_surface_pong(surface, serial);
}

static void shell_ignore_configure(void *data, struct wl_shell_surface *surface,
                                   uint32_t edges, int32_t width, int32_t height)
{
    (void)data;
    (void)surface;
    (void)edges;
    (void)width;
    (void)height;
}

static void shell_popup_done(void *data, struct wl_shell_surface *surface)
{
    (void)data;
    (void)surface;
}

static const struct wl_shell_surface_listener shell_listener = {
    shell_ping,
    shell_ignore_configure,
    shell_popup_done,
};

static void bind_named(void)
{
    if (!g.registry)
        return;
    if (!g.shell && host_shell_name) {
        g.shell = wl_registry_bind(g.registry, host_shell_name, &wl_shell_interface, 1);
        log_msg("shell bind %p", (void *)g.shell);
    }
    if (!g.webos && host_webos_name) {
        g.webos = wl_registry_bind(g.registry, host_webos_name, &wl_webos_shell_interface, 1);
        log_msg("webos bind %p", (void *)g.webos);
    }
    if (!g.input && host_input_name) {
        g.input = wl_registry_bind(g.registry, host_input_name,
                                   &wl_webos_input_manager_interface, 1);
        log_msg("input manager %p", (void *)g.input);
    }
    if (!our_subcompositor && host_subcompositor_name) {
        our_subcompositor = wl_registry_bind(g.registry, host_subcompositor_name,
                                             &wl_subcompositor_interface, 1);
        log_msg("subcompositor %p", (void *)our_subcompositor);
    }
}

/* xdg_positioner anchor/gravity enums share this numbering. */
enum { A_NONE = 0, A_TOP, A_BOTTOM, A_LEFT, A_RIGHT,
       A_TOP_LEFT, A_BOTTOM_LEFT, A_TOP_RIGHT, A_BOTTOM_RIGHT };

static void anchor_point(const struct virt *pos, int32_t *out_x, int32_t *out_y)
{
    int32_t x = pos->p_ax;
    int32_t y = pos->p_ay;

    switch (pos->p_anchor) {
    case A_TOP:          x += pos->p_aw / 2; break;
    case A_BOTTOM:       x += pos->p_aw / 2; y += pos->p_ah; break;
    case A_LEFT:         y += pos->p_ah / 2; break;
    case A_RIGHT:        x += pos->p_aw;     y += pos->p_ah / 2; break;
    case A_TOP_LEFT:     break;
    case A_BOTTOM_LEFT:  y += pos->p_ah; break;
    case A_TOP_RIGHT:    x += pos->p_aw; break;
    case A_BOTTOM_RIGHT: x += pos->p_aw;     y += pos->p_ah; break;
    default:             x += pos->p_aw / 2; y += pos->p_ah / 2; break;
    }
    *out_x = x;
    *out_y = y;
}

/* Resolve the positioner into a parent-relative x,y for the subsurface. */
static void place_popup(struct virt *popup, const struct virt *pos)
{
    int32_t ax, ay, x, y;
    int32_t w = pos->p_w > 0 ? pos->p_w : 1;
    int32_t h = pos->p_h > 0 ? pos->p_h : 1;

    anchor_point(pos, &ax, &ay);
    switch (pos->p_gravity) {
    case A_TOP:          x = ax - w / 2; y = ay - h;     break;
    case A_BOTTOM:       x = ax - w / 2; y = ay;         break;
    case A_LEFT:         x = ax - w;     y = ay - h / 2; break;
    case A_RIGHT:        x = ax;         y = ay - h / 2; break;
    case A_TOP_LEFT:     x = ax - w;     y = ay - h;     break;
    case A_BOTTOM_LEFT:  x = ax - w;     y = ay;         break;
    case A_TOP_RIGHT:    x = ax;         y = ay - h;     break;
    case A_BOTTOM_RIGHT: x = ax;         y = ay;         break;
    default:             x = ax - w / 2; y = ay - h / 2; break;
    }
    x += pos->p_ox;
    y += pos->p_oy;
    /* Stand in for constraint_adjustment: keep it on screen. */
    if (x + w > win_w)
        x = win_w - w;
    if (y + h > win_h)
        y = win_h - h;
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    popup->popup_x = x;
    popup->popup_y = y;
    popup->p_w = w;
    popup->p_h = h;
}

/* A popup surface with no role would be mapped by LSM as a nameless card,
 * which minimizes the browser. Make it a subsurface of the parent instead:
 * the compositor composites it inside our window, so it takes no card. */
static void emit_popup_configure(struct virt *popup)
{
    struct xdg_popup_listener *pl;
    struct virt *surface = NULL;
    struct xdg_surface_listener *sl;
    int i;

    if (!popup || !popup->listener || popup->configured || emitting)
        return;
    emitting = 1;
    for (i = 0; i < MAX_VIRT; i++) {
        if (g.virts[i].kind == V_SURFACE && g.virts[i].wl_surface == popup->wl_surface) {
            surface = &g.virts[i];
            break;
        }
    }
    pl = (struct xdg_popup_listener *)popup->listener;
    if (pl->configure) {
        pl->configure(popup->data, (struct xdg_popup *)popup->proxy,
                      popup->popup_x, popup->popup_y, popup->p_w, popup->p_h);
    }
    if (surface && surface->listener) {
        sl = (struct xdg_surface_listener *)surface->listener;
        if (sl->configure)
            sl->configure(surface->data, (struct xdg_surface *)surface->proxy, ++g.serial);
    }
    popup->configured = 1;
    emitting = 0;
    log_msg("popup configure %d,%d %dx%d", popup->popup_x, popup->popup_y,
            popup->p_w, popup->p_h);
}

static const char *our_app_id(void)
{
    const char *appid = getenv("APPID");

    if (!appid || !appid[0])
        appid = "com.github.gprot42.geckotv";
    return appid;
}

static void tag_child_surface(struct wl_proxy *surface)
{
    struct wl_webos_shell_surface *ss;
    int i;
    int slot = -1;

    if (!surface)
        return;
    for (i = 0; i < MAX_TAGGED; i++) {
        if (tagged_surfaces[i] == surface)
            return;
        if (!tagged_surfaces[i] && slot < 0)
            slot = i;
    }
    if (slot < 0) {
        log_msg("tag table full");
        return;
    }
    bind_named();
    if (!g.webos)
        return;
    ss = wl_webos_shell_get_shell_surface(g.webos, (struct wl_surface *)surface);
    if (!ss)
        return;
    tagged_surfaces[slot] = surface;
    /* An appId keeps it out of the nameless-card path, and the type keeps
     * it out of FullscreenView. No model claims this type. */
    wl_webos_shell_surface_set_property(ss, "appId", our_app_id());
    wl_webos_shell_surface_set_property(ss, "_WEBOS_WINDOW_TYPE",
                                        "_WEBOS_WINDOW_TYPE_SUBSURFACE");
    wl_webos_shell_surface_set_property(ss, "displayAffinity", "0");
    log_msg("tagged child surface %p, not a card", (void *)surface);
}

static void emit_toplevel_configure(struct virt *toplevel)
{
    struct virt *surface = NULL;
    struct xdg_toplevel_listener *tl;
    struct xdg_surface_listener *sl;
    struct wl_array states;
    uint32_t *state;
    int i;

    if (!toplevel || !toplevel->listener || emitting)
        return;
    emitting = 1;
    for (i = 0; i < MAX_VIRT; i++) {
        if (g.virts[i].kind == V_SURFACE && g.virts[i].wl_surface == toplevel->wl_surface) {
            surface = &g.virts[i];
            break;
        }
    }
    g.serial++;
    /* Fullscreen state makes Firefox slide the tab strip and address bar
     * off the top. The compositor still shows the surface full-screen via
     * wl_webos_shell; tell the client it is a maximized window instead. */
    wl_array_init(&states);
    state = wl_array_add(&states, sizeof *state);
    if (state)
        *state = XDG_TOPLEVEL_STATE_MAXIMIZED;
    state = wl_array_add(&states, sizeof *state);
    if (state)
        *state = XDG_TOPLEVEL_STATE_ACTIVATED;
    tl = (struct xdg_toplevel_listener *)toplevel->listener;
    if (tl->configure) {
        tl->configure(toplevel->data, (struct xdg_toplevel *)toplevel->proxy,
                      win_w, win_h, &states);
    }
    wl_array_release(&states);
    if (surface && surface->listener) {
        sl = (struct xdg_surface_listener *)surface->listener;
        if (sl->configure)
            sl->configure(surface->data, (struct xdg_surface *)surface->proxy, g.serial);
    }
    toplevel->configured = 1;
    emitting = 0;
    log_msg("configure serial %u", g.serial);
}

static void map_main_surface(struct virt *xdg_surface, struct wl_proxy *wl_surface)
{
    const char *appid;
    int fullscreen;

    if (!xdg_surface || xdg_surface->shell_surface)
        return;
    bind_named();
    xdg_surface->wl_surface = wl_surface;
    if (!g.shell || !wl_surface) {
        log_msg("cannot map shell=%p surface=%p", (void *)g.shell, (void *)wl_surface);
        return;
    }
    xdg_surface->shell_surface = wl_shell_get_shell_surface(g.shell, (struct wl_surface *)wl_surface);
    if (!xdg_surface->shell_surface) {
        log_msg("wl_shell_get_shell_surface failed");
        return;
    }
    wl_shell_surface_add_listener(xdg_surface->shell_surface, &shell_listener, NULL);
    wl_shell_surface_set_toplevel(xdg_surface->shell_surface);
    fullscreen = !mapped_one;
    main_surface = (struct wl_surface *)wl_surface;
    mapped_one = 1;
    text_input_enter_main();
    if (g.webos) {
        xdg_surface->webos_surface = wl_webos_shell_get_shell_surface(
            g.webos, (struct wl_surface *)wl_surface);
        appid = our_app_id();
        if (xdg_surface->webos_surface) {
            wl_webos_shell_surface_set_property(xdg_surface->webos_surface, "appId", appid);
            wl_webos_shell_surface_set_property(xdg_surface->webos_surface, "title", "Firefox");
            wl_webos_shell_surface_set_property(xdg_surface->webos_surface, "displayAffinity", "0");
            wl_webos_shell_surface_set_property(xdg_surface->webos_surface,
                                                "_WEBOS_ACCESS_POLICY_KEYS_BACK", "true");
            /* Back is omitted from the default mask, so a wheel-down becomes
             * the TV's exit arrow. Claim Back for this window. */
            wl_webos_shell_surface_set_key_mask(xdg_surface->webos_surface, 0xFFFFFFFAu);
            /* -1 keeps the Magic Remote pointer on screen. */
            wl_webos_shell_surface_set_property(xdg_surface->webos_surface,
                                                "_WEBOS_CURSOR_SLEEP_TIME", "-1");
            if (fullscreen) {
                wl_webos_shell_surface_set_state(xdg_surface->webos_surface,
                                                 WL_WEBOS_SHELL_SURFACE_STATE_FULLSCREEN);
            }
        }
    }
    log_msg("mapped shell=%p webos=%p fullscreen=%d", (void *)xdg_surface->shell_surface,
            (void *)xdg_surface->webos_surface, fullscreen);
}

/* Undo what the adapter created for a virtual object and free its slot. */
static void virt_teardown(struct virt *v)
{
    if (v->subsurface) {
        if (synth_focus == (struct wl_surface *)v->wl_surface)
            synth_focus = NULL;
        wl_subsurface_destroy(v->subsurface);
        v->subsurface = NULL;
    }
    if (v->shell_surface) {
        wl_shell_surface_destroy(v->shell_surface);
        v->shell_surface = NULL;
    }
    v->proxy = NULL;
}

/* text-input-v3, offered to GTK by the adapter. GTK enables text input when
 * an editable element gains focus and disables it when focus leaves, which is
 * exactly when the webOS keyboard should come and go; the webOS compositor
 * does not offer the protocol itself. The keyboard only opens for a focus
 * that follows a click, so address-bar focus on a new tab or a page's
 * autofocus does not pop it up. Text still arrives through text_model and
 * inject_text(). */
#define TEXT_INPUT_NAME 0x7f000002
static struct wl_proxy *text_input_v3;
static int text_input_bound;
static int text_input_pending, text_input_enabled;
static uint32_t text_input_commits;
static int text_input_entered;
static struct timespec last_click;

static void note_click(void)
{
    clock_gettime(CLOCK_MONOTONIC, &last_click);
}

static int clicked_recently(void)
{
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - last_click.tv_sec) * 1000 +
           (now.tv_nsec - last_click.tv_nsec) / 1000000 < 1500;
}

/* GTK only enables text input on a surface it has been told it entered. */
static void text_input_enter_main(void)
{
    struct virt *v = text_input_v3 ? virt_get(text_input_v3) : NULL;
    const struct zwp_text_input_v3_listener *l;

    if (!v || !v->listener || !main_surface || text_input_entered)
        return;
    l = v->listener;
    text_input_entered = 1;
    if (l->enter)
        l->enter(v->data, (struct zwp_text_input_v3 *)v->proxy, main_surface);
    log_msg("text-input enter main surface");
}

static void text_input_commit(struct virt *v)
{
    const struct zwp_text_input_v3_listener *l = v->listener;

    text_input_commits++;
    if (text_input_pending != text_input_enabled) {
        text_input_enabled = text_input_pending;
        log_msg("text-input %s", text_input_enabled ? "enabled" : "disabled");
        if (!text_input_enabled)
            hide_keyboard();
        else if (clicked_recently())
            show_keyboard();
    } else if (text_input_enabled && !keyboard_up && clicked_recently()) {
        /* A click on the field that already has focus (after Back closed
         * the keyboard) only moves the cursor: reopen the keyboard. */
        show_keyboard();
    }
    if (l && l->done)
        l->done(v->data, (struct zwp_text_input_v3 *)v->proxy, text_input_commits);
}

static struct wl_proxy *virt_request(struct virt *v, uint32_t opcode, uint32_t flags,
                                     const struct wl_interface *interface,
                                     union wl_argument *args)
{
    struct wl_proxy *created;
    struct virt *child;

    if (v->kind == V_WM && opcode == XDG_WM_BASE_GET_XDG_SURFACE) {
        created = new_virt(v->proxy, interface, v->version, V_SURFACE, &child);
        if (child && args)
            child->wl_surface = (struct wl_proxy *)args[1].o;
        return created;
    }
    if (v->kind == V_WM && opcode == XDG_WM_BASE_CREATE_POSITIONER) {
        return new_virt(v->proxy, interface, v->version, V_POSITIONER, NULL);
    }
    if (v->kind == V_TEXT_INPUT_MANAGER && opcode == ZWP_TEXT_INPUT_MANAGER_V3_GET_TEXT_INPUT) {
        created = new_virt(v->proxy, interface, v->version, V_TEXT_INPUT, NULL);
        text_input_v3 = created;
        text_input_entered = 0;
        log_msg("text-input created");
        return created;
    }
    if (v->kind == V_TEXT_INPUT) {
        if (opcode == ZWP_TEXT_INPUT_V3_ENABLE)
            text_input_pending = 1;
        else if (opcode == ZWP_TEXT_INPUT_V3_DISABLE)
            text_input_pending = 0;
        else if (opcode == ZWP_TEXT_INPUT_V3_COMMIT)
            text_input_commit(v);
        else if (opcode == 0 && v->proxy == text_input_v3)
            text_input_v3 = NULL;
    }
    if (v->kind == V_POSITIONER && args) {
        switch (opcode) {
        case XDG_POSITIONER_SET_SIZE:
            v->p_w = args[0].i; v->p_h = args[1].i; break;
        case XDG_POSITIONER_SET_ANCHOR_RECT:
            v->p_ax = args[0].i; v->p_ay = args[1].i;
            v->p_aw = args[2].i; v->p_ah = args[3].i; break;
        case XDG_POSITIONER_SET_ANCHOR:
            v->p_anchor = args[0].u; break;
        case XDG_POSITIONER_SET_GRAVITY:
            v->p_gravity = args[0].u; break;
        case XDG_POSITIONER_SET_OFFSET:
            v->p_ox = args[0].i; v->p_oy = args[1].i; break;
        default:
            break;
        }
    }
    if (v->kind == V_SURFACE && opcode == XDG_SURFACE_GET_TOPLEVEL) {
        /* Only a toplevel takes the card role. A popup surface gets no
         * shell surface at all, so LSM cannot mistake it for a card. */
        map_main_surface(v, v->wl_surface);
        created = new_virt(v->proxy, interface, v->version, V_TOPLEVEL, &child);
        if (child)
            child->wl_surface = v->wl_surface;
        return created;
    }
    if (v->kind == V_SURFACE && opcode == XDG_SURFACE_GET_POPUP) {
        struct virt *parent = args ? virt_get((struct wl_proxy *)args[1].o) : NULL;
        struct virt *pos = args ? virt_get((struct wl_proxy *)args[2].o) : NULL;

        created = new_virt(v->proxy, interface, v->version, V_POPUP, &child);
        if (child) {
            child->wl_surface = v->wl_surface;
            if (pos)
                place_popup(child, pos);
            {
                int i;
                int32_t ox = 0, oy = 0;
                /* A submenu's parent is another popup, so add its offset. */
                for (i = 0; parent && i < MAX_VIRT; i++) {
                    if (g.virts[i].proxy && g.virts[i].kind == V_POPUP &&
                        g.virts[i].wl_surface == parent->wl_surface) {
                        ox = g.virts[i].abs_x;
                        oy = g.virts[i].abs_y;
                        break;
                    }
                }
                child->abs_x = ox + child->popup_x;
                child->abs_y = oy + child->popup_y;
            }
            bind_named();
            if (our_subcompositor && parent && parent->wl_surface && v->wl_surface) {
                child->subsurface = wl_subcompositor_get_subsurface(
                    our_subcompositor,
                    (struct wl_surface *)v->wl_surface,
                    (struct wl_surface *)parent->wl_surface);
                if (child->subsurface) {
                    wl_subsurface_set_position(child->subsurface,
                                               child->popup_x, child->popup_y);
                    wl_subsurface_set_desync(child->subsurface);
                }
            }
            log_msg("popup surface=%p sub=%p at %d,%d %dx%d parent=%p",
                    (void *)child->wl_surface,
                    (void *)child->subsurface, child->popup_x, child->popup_y,
                    child->p_w, child->p_h, (void *)(parent ? parent->wl_surface : NULL));
        }
        return created;
    }
    if (v->kind == V_POPUP && opcode == XDG_POPUP_REPOSITION && args) {
        struct virt *pos = virt_get((struct wl_proxy *)args[0].o);
        if (pos) {
            place_popup(v, pos);
            if (v->subsurface)
                wl_subsurface_set_position(v->subsurface, v->popup_x, v->popup_y);
            v->configured = 0;
            emit_popup_configure(v);
        }
        return NULL;
    }
    if (v->kind == V_TOPLEVEL && opcode == XDG_TOPLEVEL_SET_TITLE && args && args[0].s) {
        int i;
        for (i = 0; i < MAX_VIRT; i++) {
            if (g.virts[i].kind == V_SURFACE && g.virts[i].wl_surface == v->wl_surface &&
                g.virts[i].webos_surface) {
                wl_webos_shell_surface_set_property(g.virts[i].webos_surface, "title", args[0].s);
                break;
            }
        }
    }
    if (v->kind == V_TOPLEVEL &&
        (opcode == XDG_TOPLEVEL_SET_FULLSCREEN || opcode == XDG_TOPLEVEL_SET_MAXIMIZED))
        emit_toplevel_configure(v);
    if (opcode == 0 && v->proxy) {
        struct wl_proxy *dying = v->proxy;

        virt_teardown(v);
        /* Code generated by wayland-scanner 1.20 and later destroys in the
         * same call (WL_MARSHAL_FLAG_DESTROY). Older code, such as Debian
         * 11's GTK, sends the request and then calls wl_proxy_destroy(),
         * which frees the proxy in webos_xdg_virtual_destroyed(). */
        if (flags & WL_MARSHAL_FLAG_DESTROY)
            webos_xdg_destroy_virtual(dying);
    }
    return NULL;
}

/* Called by our libwayland for wl_proxy_destroy() on a proxy with id 0,
 * which only virtual proxies have. */
void webos_xdg_virtual_destroyed(struct wl_proxy *proxy)
{
    struct virt *v = virt_get(proxy);

    if (v && v->proxy == proxy)
        virt_teardown(v);
    webos_xdg_destroy_virtual(proxy);
}

static void wrapped_global(void *data, struct wl_registry *registry, uint32_t name,
                           const char *interface, uint32_t version)
{
    struct virt *reg = virt_get((struct wl_proxy *)registry);
    void (*user_global)(void *, struct wl_registry *, uint32_t, const char *, uint32_t);
    uint32_t client_version = version;
    static int outputs;

    user_global = (reg && reg->listener)
        ? ((void (**)(void *, struct wl_registry *, uint32_t, const char *, uint32_t))reg->listener)[0]
        : NULL;
    if (!strcmp(interface, "wl_shell"))
        host_shell_name = name;
    if (!strcmp(interface, "wl_webos_shell"))
        host_webos_name = name;
    if (!strcmp(interface, "wl_webos_input_manager"))
        host_input_name = name;
    if (!strcmp(interface, "text_model_factory"))
        host_text_name = name;
    if (!strcmp(interface, "wl_seat") && !host_seat_name)
        host_seat_name = name;
    if (!strcmp(interface, "wl_shm") && !host_shm_name)
        host_shm_name = name;
    if (!strcmp(interface, "wl_subcompositor") && !host_subcompositor_name)
        host_subcompositor_name = name;
    if (!strcmp(interface, "wl_compositor")) {
        if (!host_compositor_name)
            host_compositor_name = name;
        server_compositor_version = version;
        if (client_version < 4)
            client_version = 4;
        log_msg("global wl_compositor server=%u client=%u", version, client_version);
    }
    if (!strcmp(interface, "wl_output")) {
        server_output_version = version;
        if (client_version < 3)
            client_version = 3;
        log_msg("global wl_output server=%u client=%u", version, client_version);
    }
    if (!strcmp(interface, "wl_output") && outputs++ > 0)
        return;
    if (user_global)
        user_global(data, registry, name, interface, client_version);
    if (!strcmp(interface, "wl_compositor") && user_global && (!reg || !reg->injected)) {
        if (reg)
            reg->injected = 1;
        g.xdg_name = 0x7f000001;
        log_msg("inject xdg_wm_base");
        user_global(data, registry, g.xdg_name, "xdg_wm_base", 4);
        log_msg("inject zwp_text_input_manager_v3");
        user_global(data, registry, TEXT_INPUT_NAME, "zwp_text_input_manager_v3", 1);
    }
}

static void wrapped_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    struct virt *reg = virt_get((struct wl_proxy *)registry);
    void (*user_remove)(void *, struct wl_registry *, uint32_t);

    user_remove = (reg && reg->listener)
        ? ((void (**)(void *, struct wl_registry *, uint32_t))reg->listener)[1]
        : NULL;
    if (user_remove)
        user_remove(data, registry, name);
}

static const struct wl_registry_listener registry_wrapper = {
    wrapped_global,
    wrapped_remove,
};

static struct hook *hook_slot(struct hook *table, struct wl_proxy *proxy)
{
    int i;
    struct hook *free_slot = NULL;

    for (i = 0; i < MAX_HOOK; i++) {
        if (table[i].proxy == proxy) {
            return &table[i];
        }
        if (!table[i].proxy && !free_slot)
            free_slot = &table[i];
    }
    return free_slot;
}

static struct hook *hook_find(struct hook *table, const void *proxy)
{
    int i;

    for (i = 0; i < MAX_HOOK; i++) {
        if (table[i].proxy == proxy)
            return &table[i];
    }
    return NULL;
}

/* US keyboard evdev codes. The TV keymap is evdev/us, so Firefox turns
 * these plus the shift modifier into the typed character. */
static uint32_t letter_code(char lower)
{
    const char *row1 = "qwertyuiop";
    const char *row2 = "asdfghjkl";
    const char *row3 = "zxcvbnm";
    const char *found;

    found = strchr(row1, lower);
    if (found)
        return 16 + (uint32_t)(found - row1);
    found = strchr(row2, lower);
    if (found)
        return 30 + (uint32_t)(found - row2);
    found = strchr(row3, lower);
    if (found)
        return 44 + (uint32_t)(found - row3);
    return 0;
}

static int ascii_key(char ch, uint32_t *code, int *shift)
{
    *shift = 0;
    if (ch >= 'a' && ch <= 'z') {
        *code = letter_code(ch);
        return *code != 0;
    }
    if (ch >= 'A' && ch <= 'Z') {
        *code = letter_code((char)(ch - 'A' + 'a'));
        *shift = 1;
        return *code != 0;
    }
    if (ch >= '1' && ch <= '9') {
        *code = 2 + (uint32_t)(ch - '1');
        return 1;
    }
    switch (ch) {
    case '0': *code = 11; return 1;
    case '-': *code = 12; return 1;
    case '_': *code = 12; *shift = 1; return 1;
    case '=': *code = 13; return 1;
    case '+': *code = 13; *shift = 1; return 1;
    case '\n':
    case '\r': *code = 28; return 1;
    case '\b': *code = 14; return 1;
    case '\t': *code = 15; return 1;
    case '[': *code = 26; return 1;
    case '{': *code = 26; *shift = 1; return 1;
    case ']': *code = 27; return 1;
    case '}': *code = 27; *shift = 1; return 1;
    case ';': *code = 39; return 1;
    case ':': *code = 39; *shift = 1; return 1;
    case '\'': *code = 40; return 1;
    case '"': *code = 40; *shift = 1; return 1;
    case '`': *code = 41; return 1;
    case '~': *code = 41; *shift = 1; return 1;
    case '\\': *code = 43; return 1;
    case '|': *code = 43; *shift = 1; return 1;
    case ',': *code = 51; return 1;
    case '<': *code = 51; *shift = 1; return 1;
    case '.': *code = 52; return 1;
    case '>': *code = 52; *shift = 1; return 1;
    case '/': *code = 53; return 1;
    case '?': *code = 53; *shift = 1; return 1;
    case ' ': *code = 57; return 1;
    case '!': *code = 2; *shift = 1; return 1;
    case '@': *code = 3; *shift = 1; return 1;
    case '#': *code = 4; *shift = 1; return 1;
    case '$': *code = 5; *shift = 1; return 1;
    case '%': *code = 6; *shift = 1; return 1;
    case '^': *code = 7; *shift = 1; return 1;
    case '&': *code = 8; *shift = 1; return 1;
    case '*': *code = 9; *shift = 1; return 1;
    case '(': *code = 10; *shift = 1; return 1;
    case ')': *code = 11; *shift = 1; return 1;
    default: return 0;
    }
}

static void inject_key(uint32_t code, int shift)
{
    const struct wl_keyboard_listener *keys;
    uint32_t serial;

    if (!focused_keys || !focused_keys->listener)
        return;
    keys = focused_keys->listener;
    if (!keys->key)
        return;
    serial = ++g.serial;
    if (shift && keys->modifiers)
        keys->modifiers(focused_keys->data, (struct wl_keyboard *)focused_keys->proxy,
                        serial, 1, 0, 0, 0);
    keys->key(focused_keys->data, (struct wl_keyboard *)focused_keys->proxy,
              serial, 0, code, WL_KEYBOARD_KEY_STATE_PRESSED);
    keys->key(focused_keys->data, (struct wl_keyboard *)focused_keys->proxy,
              serial, 0, code, WL_KEYBOARD_KEY_STATE_RELEASED);
    if (shift && keys->modifiers)
        keys->modifiers(focused_keys->data, (struct wl_keyboard *)focused_keys->proxy,
                        serial, 0, 0, 0, 0);
}

static void inject_text(const char *text)
{
    const char *p;

    if (!text)
        return;
    log_msg("type \"%s\"", text);
    for (p = text; *p; p++) {
        uint32_t code;
        int shift;
        if (ascii_key(*p, &code, &shift))
            inject_key(code, shift);
    }
}

static void focus_address_bar(void)
{
    /* Control is xkb modifier 2. L is evdev 38. This selects the URL. */
    const struct wl_keyboard_listener *keys;
    uint32_t serial;

    if (!focused_keys || !focused_keys->listener)
        return;
    keys = focused_keys->listener;
    if (!keys->key)
        return;
    serial = ++g.serial;
    if (keys->modifiers)
        keys->modifiers(focused_keys->data, (struct wl_keyboard *)focused_keys->proxy,
                        serial, 4, 0, 0, 0);
    keys->key(focused_keys->data, (struct wl_keyboard *)focused_keys->proxy,
              serial, 0, 38, WL_KEYBOARD_KEY_STATE_PRESSED);
    keys->key(focused_keys->data, (struct wl_keyboard *)focused_keys->proxy,
              serial, 0, 38, WL_KEYBOARD_KEY_STATE_RELEASED);
    if (keys->modifiers)
        keys->modifiers(focused_keys->data, (struct wl_keyboard *)focused_keys->proxy,
                        serial, 0, 0, 0, 0);
    log_msg("focus address bar");
}

static void show_keyboard(void)
{
    ensure_text_model();
    if (!text_input || !our_seat || !main_surface)
        return;
    text_model_set_content_type(text_input, 0x100, 5);
    text_model_set_enter_key_type(text_input, 3);
    text_model_set_surrounding_text(text_input, "", 0, 0);
    text_model_activate(text_input, ++g.serial, our_seat, main_surface);
    text_model_show_input_panel(text_input);
    keyboard_up = 1;
    log_msg("keyboard shown");
}

/* Nothing else dismisses the panel. The IME keeps it up until the client
 * asks, so Back, a click outside the address bar, and text_leave all land
 * here. Deactivating as well as hiding stops the IME reopening it on the
 * next focus event. */
static void hide_keyboard(void)
{
    if (!text_input || !keyboard_up)
        return;
    text_model_hide_input_panel(text_input);
    if (our_seat)
        text_model_deactivate(text_input, our_seat);
    keyboard_up = 0;
    log_msg("keyboard hidden");
}

static void text_commit_string(void *data, struct text_model *model, uint32_t serial,
                               const char *text)
{
    (void)data;
    (void)model;
    (void)serial;
    inject_text(text);
}

static void text_preedit_string(void *data, struct text_model *model, uint32_t serial,
                                const char *text, const char *commit)
{
    (void)data;
    (void)model;
    (void)serial;
    (void)text;
    (void)commit;
}

static void text_delete_surrounding(void *data, struct text_model *model, uint32_t serial,
                                    int32_t index, uint32_t length)
{
    uint32_t i;

    (void)data;
    (void)model;
    (void)serial;
    if (index < 0)
        length = length ? length : (uint32_t)(-index);
    for (i = 0; i < length; i++)
        inject_key(14, 0);
}

static void text_cursor_position(void *data, struct text_model *model, uint32_t serial,
                                 int32_t index, int32_t anchor)
{
    (void)data;
    (void)model;
    (void)serial;
    (void)index;
    (void)anchor;
}

static void text_preedit_styling(void *data, struct text_model *model, uint32_t serial,
                                 uint32_t index, uint32_t length, uint32_t style)
{
    (void)data;
    (void)model;
    (void)serial;
    (void)index;
    (void)length;
    (void)style;
}

static void text_preedit_cursor(void *data, struct text_model *model, uint32_t serial,
                                int32_t index)
{
    (void)data;
    (void)model;
    (void)serial;
    (void)index;
}

static void text_modifiers_map(void *data, struct text_model *model, struct wl_array *map)
{
    (void)data;
    (void)model;
    (void)map;
}

static void text_keysym(void *data, struct text_model *model, uint32_t serial, uint32_t time,
                        uint32_t sym, uint32_t state, uint32_t modifiers)
{
    (void)data;
    (void)model;
    (void)serial;
    (void)time;
    (void)modifiers;
    if (state != WL_KEYBOARD_KEY_STATE_PRESSED)
        return;
    /* The webOS keyboard sends editing keys as X11 keysyms rather than as
     * text or delete_surrounding_text (it has no surrounding text from us):
     * BackSpace arrives as 0xff08. Press the matching evdev key. */
    switch (sym) {
    case 0xff08: inject_key(KEY_BACKSPACE, 0); break;
    case 0xff09: inject_key(KEY_TAB, 0); break;
    case 0xff0d: /* Return */
    case 0xff8d: inject_key(KEY_ENTER, 0); break; /* KP_Enter */
    case 0xff1b: inject_key(KEY_ESC, 0); break;
    case 0xff50: inject_key(KEY_HOME, 0); break;
    case 0xff51: inject_key(KEY_LEFT, 0); break;
    case 0xff52: inject_key(KEY_UP, 0); break;
    case 0xff53: inject_key(KEY_RIGHT, 0); break;
    case 0xff54: inject_key(KEY_DOWN, 0); break;
    case 0xff57: inject_key(KEY_END, 0); break;
    case 0xffff: inject_key(KEY_DELETE, 0); break;
    default:
        log_msg("keysym %#x not mapped", sym);
        return;
    }
    log_msg("keysym %#x", sym);
}

static void text_enter(void *data, struct text_model *model, struct wl_surface *surface)
{
    (void)data;
    (void)model;
    (void)surface;
}

static void text_leave(void *data, struct text_model *model)
{
    (void)data;
    (void)model;
    keyboard_up = 0;
}

/* Where the webOS keyboard sits on screen, in the remote's 1920x1080 grid;
 * zero size until the keyboard reports it. */
static int32_t panel_x, panel_y, panel_w, panel_h;

static void text_panel_state(void *data, struct text_model *model, uint32_t state)
{
    (void)data;
    (void)model;
    log_msg("keyboard panel %u", state);
    /* Closed by its own button, not by us. */
    if (state == 0)
        keyboard_up = 0;
}

static void text_panel_rect(void *data, struct text_model *model, int32_t x, int32_t y,
                            uint32_t width, uint32_t height)
{
    (void)data;
    (void)model;
    panel_x = x;
    panel_y = y;
    panel_w = (int32_t)width;
    panel_h = (int32_t)height;
    log_msg("keyboard panel at %d,%d %ux%u", x, y, width, height);
}

/* True if a remote position (1920x1080 grid) is on the open keyboard. Clicks
 * there go to the keyboard: the compositor routes them to its surface, not
 * ours, so they must not be injected into Firefox as well. Before the
 * keyboard reports its area, assume it covers the bottom 45%. */
static int on_keyboard(int32_t x, int32_t y)
{
    if (!keyboard_up)
        return 0;
    if (panel_w > 0 && panel_h > 0)
        return x >= panel_x && x < panel_x + panel_w && y >= panel_y && y < panel_y + panel_h;
    return y >= 1080 * 55 / 100;
}

static const struct text_model_listener text_listener = {
    text_commit_string,
    text_preedit_string,
    text_delete_surrounding,
    text_cursor_position,
    text_preedit_styling,
    text_preedit_cursor,
    text_modifiers_map,
    text_keysym,
    text_enter,
    text_leave,
    text_panel_state,
    text_panel_rect,
};

static void ensure_text_model(void)
{
    struct text_model_factory *factory;

    if (!g.registry)
        return;
    if (!our_seat && host_seat_name) {
        our_seat = wl_registry_bind(g.registry, host_seat_name, &wl_seat_interface, 4);
        log_msg("our seat %p", (void *)our_seat);
    }
    if (text_input || !host_text_name)
        return;
    factory = wl_registry_bind(g.registry, host_text_name, &text_model_factory_interface, 1);
    if (!factory)
        return;
    text_input = text_model_factory_create_text_model(factory);
    if (text_input)
        text_model_add_listener(text_input, &text_listener, NULL);
    log_msg("text model %p", (void *)text_input);
}

static void pointer_enter(void *data, struct wl_pointer *pointer, uint32_t serial,
                          struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y)
{
    struct hook *hook = hook_find(pointer_hooks, pointer);
    const struct wl_pointer_listener *orig = hook ? hook->listener : NULL;

    last_x = x >> 8;
    last_y = y >> 8;
    saw_motion = 1;
    synth_focus = surface;
    note_real_pointer();
    log_msg("POINTER enter surface=%p at %d,%d", (void *)surface, last_x, last_y);
    if (orig && orig->enter)
        orig->enter(data, pointer, serial, surface, x, y);
}

static void pointer_leave(void *data, struct wl_pointer *pointer, uint32_t serial,
                          struct wl_surface *surface)
{
    struct hook *hook = hook_find(pointer_hooks, pointer);
    const struct wl_pointer_listener *orig = hook ? hook->listener : NULL;

    if (synth_focus == surface)
        synth_focus = NULL;
    log_msg("POINTER leave surface=%p", (void *)surface);
    if (orig && orig->leave)
        orig->leave(data, pointer, serial, surface);
}

static void pointer_motion(void *data, struct wl_pointer *pointer, uint32_t time,
                           wl_fixed_t x, wl_fixed_t y)
{
    struct hook *hook = hook_find(pointer_hooks, pointer);
    const struct wl_pointer_listener *orig = hook ? hook->listener : NULL;

    last_x = x >> 8;
    last_y = y >> 8;
    saw_motion = 1;
    note_real_pointer();
    if (orig && orig->motion)
        orig->motion(data, pointer, time, x, y);
}

static void pointer_button(void *data, struct wl_pointer *pointer, uint32_t serial,
                           uint32_t time, uint32_t button, uint32_t state)
{
    struct hook *hook = hook_find(pointer_hooks, pointer);
    const struct wl_pointer_listener *orig = hook ? hook->listener : NULL;

    note_real_pointer();
    log_msg("click button %u state %u x %d y %d", button, state, last_x, last_y);
    if (orig && orig->button)
        orig->button(data, pointer, serial, time, button, state);
    /* Address field sits under the tab strip and left of the menu button.
     * Coordinates are surface-local, so only judge them on the main window;
     * inside a menu they mean something else entirely. */
    if (state == WL_POINTER_BUTTON_STATE_PRESSED)
        note_click();
    /* Without text-input-v3 (GTK did not bind it), guess from the position. */
    if (!text_input_bound && state == WL_POINTER_BUTTON_STATE_PRESSED && saw_motion &&
        (!synth_focus || synth_focus == main_surface)) {
        if (last_y >= 30 && last_y < 110 && last_x < 1700)
            show_keyboard();
        else
            hide_keyboard();
    }
}

static struct hook *primary_pointer(void)
{
    int i;

    for (i = 0; i < MAX_HOOK; i++) {
        if (pointer_hooks[i].proxy && pointer_hooks[i].listener)
            return &pointer_hooks[i];
    }
    return NULL;
}

static void deliver_wheel(struct hook *hook, int linux_value, wl_fixed_t fx, wl_fixed_t fy)
{
    const struct wl_pointer_listener *keys = hook->listener;
    int notch = linux_value > 0 ? -1 : 1;
    wl_fixed_t step = (wl_fixed_t)(notch * 60) << 8;

    if (!keys)
        return;
    /* No motion here: it would move the focus off an open menu. The wheel
     * is delivered wherever the compositor has already put the pointer. */
    (void)fx;
    (void)fy;
    if (keys->axis_source)
        keys->axis_source(hook->data, (struct wl_pointer *)hook->proxy, 0);
    if (keys->axis_discrete)
        keys->axis_discrete(hook->data, (struct wl_pointer *)hook->proxy, 0, notch);
    if (keys->axis_value120)
        keys->axis_value120(hook->data, (struct wl_pointer *)hook->proxy, 0, notch * 120);
    if (keys->axis)
        keys->axis(hook->data, (struct wl_pointer *)hook->proxy, 0, 0, step);
    if (keys->frame)
        keys->frame(hook->data, (struct wl_pointer *)hook->proxy);
    log_msg("wheel seat %p axis %d frame %d discrete %d v120 %d",
            hook->proxy, keys->axis != NULL, keys->frame != NULL,
            keys->axis_discrete != NULL, keys->axis_value120 != NULL);
}

/* Topmost live popup containing the point, in screen coordinates. */
static struct virt *popup_at(int x, int y)
{
    struct virt *best = NULL;
    int i;

    for (i = 0; i < MAX_VIRT; i++) {
        struct virt *v = &g.virts[i];

        if (!v->proxy || v->kind != V_POPUP || !v->subsurface || !v->configured)
            continue;
        if (x >= v->abs_x && x < v->abs_x + v->p_w &&
            y >= v->abs_y && y < v->abs_y + v->p_h)
            best = v;
    }
    return best;
}

/* Move the seat's focus to `target` if it is not already there, then report
 * the position in that surface's own coordinates. */
static void focus_and_move(struct hook *hook, struct wl_surface *target,
                           int lx, int ly)
{
    const struct wl_pointer_listener *keys = hook->listener;

    if (!keys)
        return;
    if (target != synth_focus) {
        if (synth_focus && keys->leave)
            keys->leave(hook->data, (struct wl_pointer *)hook->proxy,
                        ++g.serial, synth_focus);
        if (target && keys->enter)
            keys->enter(hook->data, (struct wl_pointer *)hook->proxy,
                        ++g.serial, target, lx << 8, ly << 8);
        synth_focus = target;
        log_msg("pointer focus -> %p at %d,%d", (void *)target, lx, ly);
    }
    if (keys->motion)
        keys->motion(hook->data, (struct wl_pointer *)hook->proxy, 0,
                     lx << 8, ly << 8);
}

static void apply_remote_event(struct ptr_ev ev)
{
    struct hook *hook = primary_pointer();
    const struct wl_pointer_listener *keys;
    wl_fixed_t fx;
    wl_fixed_t fy;

    /* The keyboard handles its own clicks; Back and the wheel still apply. */
    if (ev.button >= 0 && on_keyboard(ev.x, ev.y))
        return;
    /* The Magic Remote reports positions on a 1920x1080 grid. */
    ev.x = ev.x * win_w / 1920;
    ev.y = ev.y * win_h / 1080;
    fx = ev.x < 0 ? 0 : ev.x;
    fy = ev.y < 0 ? 0 : ev.y;
    if (fx > win_w - 1)
        fx = win_w - 1;
    if (fy > win_h - 1)
        fy = win_h - 1;
    fx <<= 8;
    fy <<= 8;
    /* Back, read straight from evdev so it works even while the IME
     * panel holds Wayland focus. */
    if (ev.button == -3) {
        hide_keyboard();
        return;
    }
    /* The compositor closes the window when it sees this notch.
     * Hand it to every seat as a scroll instead. */
    if (ev.button == -2) {
        int i;

        for (i = 0; i < MAX_HOOK; i++) {
            if (pointer_hooks[i].proxy && pointer_hooks[i].listener) {
                deliver_wheel(&pointer_hooks[i], ev.down, fx, fy);
                break;
            }
        }
        log_msg("remote wheel %d", ev.down);
        return;
    }
    if (!hook)
        return;
    keys = hook->listener;
    /* The compositor is already sending these. A second copy breaks menus. */
    if (real_pointer_active()) {
        static int quiet;
        if (!quiet) {
            quiet = 1;
            log_msg("real pointer is live, not injecting clicks");
        }
        return;
    }
    if (ev.x < 0)
        ev.x = 0;
    if (ev.y < 0)
        ev.y = 0;
    if (ev.x > win_w - 1)
        ev.x = win_w - 1;
    if (ev.y > win_h - 1)
        ev.y = win_h - 1;
    last_x = ev.x;
    last_y = ev.y;
    saw_motion = 1;
    {
        struct virt *over = popup_at(ev.x, ev.y);
        struct wl_surface *target = over ? (struct wl_surface *)over->wl_surface
                                         : main_surface;
        int lx = over ? ev.x - over->abs_x : ev.x;
        int ly = over ? ev.y - over->abs_y : ev.y;

        focus_and_move(hook, target, lx, ly);
        if (ev.button < 0 || !keys->button)
            return;
        keys->button(hook->data, (struct wl_pointer *)hook->proxy, ++g.serial, 0,
                     BTN_LEFT, ev.down ? WL_POINTER_BUTTON_STATE_PRESSED
                                       : WL_POINTER_BUTTON_STATE_RELEASED);
        if (keys->frame)
            keys->frame(hook->data, (struct wl_pointer *)hook->proxy);
        log_msg("remote click %d at %d,%d%s", ev.down, ev.x, ev.y,
                over ? " (in popup)" : "");
        if (ev.down)
            note_click();
        /* A click inside a menu must not open or close the keyboard. */
        if (!text_input_bound && ev.down && !over) {
            if (ev.y >= 30 && ev.y < 110 && ev.x < 1700)
                show_keyboard();
            else
                hide_keyboard();
        }
    }
    (void)fx;
    (void)fy;
}

static void drain_remote(void)
{
    for (;;) {
        struct ptr_ev ev;

        pthread_mutex_lock(&ptr_mu);
        if (ptr_q_head == ptr_q_tail) {
            pthread_mutex_unlock(&ptr_mu);
            return;
        }
        ev = ptr_q[ptr_q_head];
        ptr_q_head = (ptr_q_head + 1) % PTR_QUEUE;
        pthread_mutex_unlock(&ptr_mu);
        apply_remote_event(ev);
    }
}

static void remote_sync_done(void *data, struct wl_callback *callback, uint32_t serial)
{
    (void)data;
    (void)serial;
    wl_callback_destroy(callback);
    pthread_mutex_lock(&ptr_mu);
    wake_pending_flag = 0;
    pthread_mutex_unlock(&ptr_mu);
    drain_remote();
}

static const struct wl_callback_listener remote_sync_listener = {
    remote_sync_done,
};

static void enqueue_remote(int x, int y, int button, int down)
{
    int wake = 0;

    pthread_mutex_lock(&ptr_mu);
    {
        int next = (ptr_q_tail + 1) % PTR_QUEUE;
        if (next != ptr_q_head) {
            ptr_q[ptr_q_tail].button = button;
            ptr_q[ptr_q_tail].down = down;
            ptr_q[ptr_q_tail].x = x;
            ptr_q[ptr_q_tail].y = y;
            ptr_q_tail = next;
        }
        if (!wake_pending_flag && wake_display) {
            wake_pending_flag = 1;
            wake = 1;
        }
    }
    pthread_mutex_unlock(&ptr_mu);
    if (!wake)
        return;
    {
        struct wl_callback *callback = wl_display_sync(wake_display);
        if (callback)
            wl_callback_add_listener(callback, &remote_sync_listener, NULL);
        wl_display_flush(wake_display);
    }
}

static int center_button(unsigned code)
{
    /* The wheel click and the pointer click show up as Enter, OK, or left button. */
    return code == KEY_ENTER || code == KEY_OK || code == BTN_LEFT;
}

static void *remote_thread(void *arg)
{
    const char *paths[] = {
        "/dev/input/event0",
        "/dev/input/event2",
        "/dev/input/event3",
        "/dev/input/event4",
    };
    struct pollfd pfds[4];
    int abs_x = 960;
    int abs_y = 540;
    int i;

    (void)arg;
    for (i = 0; i < 4; i++) {
        pfds[i].fd = open(paths[i], O_RDONLY | O_CLOEXEC | O_NONBLOCK);
        pfds[i].events = POLLIN;
        if (pfds[i].fd < 0)
            log_msg("remote %s errno %d", paths[i], errno);
    }
    if (pfds[0].fd < 0 && pfds[1].fd < 0)
        return NULL;
    log_msg("remote open");
    for (;;) {
        if (poll(pfds, 4, 1000) <= 0)
            continue;
        for (i = 0; i < 4; i++) {
            struct input_event ev;

            if (pfds[i].fd < 0 || !(pfds[i].revents & POLLIN))
                continue;
            while (read(pfds[i].fd, &ev, sizeof ev) == (ssize_t)sizeof ev) {
                if (ev.type == EV_ABS && ev.code == ABS_X)
                    abs_x = ev.value;
                else if (ev.type == EV_ABS && ev.code == ABS_Y)
                    abs_y = ev.value;
                else if (!mapped_one)
                    continue;
                else if (ev.type == EV_REL && ev.code == REL_WHEEL && ev.value)
                    enqueue_remote(abs_x, abs_y, -2, ev.value);
                else if (ev.type == EV_KEY && ev.code == KEY_BACK && ev.value == 1)
                    enqueue_remote(abs_x, abs_y, -3, 1);
                else if (ev.type == EV_KEY && center_button(ev.code))
                    enqueue_remote(abs_x, abs_y, BTN_LEFT, ev.value ? 1 : 0);
            }
        }
    }
    return NULL;
}

static void start_remote_pointer(void)
{
    pthread_t thread;

    if (pthread_create(&thread, NULL, remote_thread, NULL) == 0)
        pthread_detach(thread);
}

static void pointer_axis(void *data, struct wl_pointer *pointer, uint32_t time,
                         uint32_t axis, wl_fixed_t value)
{
    struct hook *hook = hook_find(pointer_hooks, pointer);
    const struct wl_pointer_listener *orig = hook ? hook->listener : NULL;

    if (orig && orig->axis)
        orig->axis(data, pointer, time, axis, value);
}

static void pointer_frame(void *data, struct wl_pointer *pointer)
{
    struct hook *hook = hook_find(pointer_hooks, pointer);
    const struct wl_pointer_listener *orig = hook ? hook->listener : NULL;

    if (orig && orig->frame)
        orig->frame(data, pointer);
}

static void pointer_axis_source(void *data, struct wl_pointer *pointer, uint32_t source)
{
    struct hook *hook = hook_find(pointer_hooks, pointer);
    const struct wl_pointer_listener *orig = hook ? hook->listener : NULL;

    if (orig && orig->axis_source)
        orig->axis_source(data, pointer, source);
}

static void pointer_axis_stop(void *data, struct wl_pointer *pointer, uint32_t time,
                              uint32_t axis)
{
    struct hook *hook = hook_find(pointer_hooks, pointer);
    const struct wl_pointer_listener *orig = hook ? hook->listener : NULL;

    if (orig && orig->axis_stop)
        orig->axis_stop(data, pointer, time, axis);
}

static void pointer_axis_discrete(void *data, struct wl_pointer *pointer, uint32_t axis,
                                  int32_t discrete)
{
    struct hook *hook = hook_find(pointer_hooks, pointer);
    const struct wl_pointer_listener *orig = hook ? hook->listener : NULL;

    if (orig && orig->axis_discrete)
        orig->axis_discrete(data, pointer, axis, discrete);
}

static void pointer_axis_value120(void *data, struct wl_pointer *pointer, uint32_t axis,
                                  int32_t value120)
{
    struct hook *hook = hook_find(pointer_hooks, pointer);
    const struct wl_pointer_listener *orig = hook ? hook->listener : NULL;

    if (orig && orig->axis_value120)
        orig->axis_value120(data, pointer, axis, value120);
}

static void pointer_axis_relative_direction(void *data, struct wl_pointer *pointer,
                                            uint32_t axis, uint32_t direction)
{
    struct hook *hook = hook_find(pointer_hooks, pointer);
    const struct wl_pointer_listener *orig = hook ? hook->listener : NULL;

    if (orig && orig->axis_relative_direction)
        orig->axis_relative_direction(data, pointer, axis, direction);
}

static const struct wl_pointer_listener pointer_wrapper = {
    pointer_enter,
    pointer_leave,
    pointer_motion,
    pointer_button,
    pointer_axis,
    pointer_frame,
    pointer_axis_source,
    pointer_axis_stop,
    pointer_axis_discrete,
    pointer_axis_value120,
    pointer_axis_relative_direction,
};

/* True while a menu is on screen. */
static int popup_is_open(void)
{
    int i;

    for (i = 0; i < MAX_VIRT; i++) {
        if (g.virts[i].proxy && g.virts[i].kind == V_POPUP && g.virts[i].subsurface)
            return 1;
    }
    return 0;
}

static int is_popup_surface(struct wl_surface *surface)
{
    int i;

    if (!surface)
        return 0;
    for (i = 0; i < MAX_VIRT; i++) {
        if (g.virts[i].proxy && g.virts[i].kind == V_POPUP &&
            g.virts[i].subsurface &&
            (struct wl_surface *)g.virts[i].wl_surface == surface)
            return 1;
    }
    return 0;
}

static void keyboard_enter(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                           struct wl_surface *surface, struct wl_array *keys)
{
    struct hook *hook = hook_find(keyboard_hooks, keyboard);
    const struct wl_keyboard_listener *orig = hook ? hook->listener : NULL;

    focused_keys = hook;
    log_msg("KEYBOARD enter surface=%p", (void *)surface);
    /* LSM treats a popup subsurface as separately focusable and hands it the
     * keyboard. GTK reads that as its toplevel losing focus and deactivates
     * the menu, destroying the popup between the press and the release, so
     * no item ever activates. Keep GTK believing the toplevel still has it. */
    if (is_popup_surface(surface)) {
        log_msg("  swallowed: popup keyboard focus stays hidden from GTK");
        return;
    }
    if (orig && orig->enter)
        orig->enter(data, keyboard, serial, surface, keys);
}

static void keyboard_leave(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                           struct wl_surface *surface)
{
    struct hook *hook = hook_find(keyboard_hooks, keyboard);
    const struct wl_keyboard_listener *orig = hook ? hook->listener : NULL;

    log_msg("KEYBOARD leave surface=%p", (void *)surface);
    if (is_popup_surface(surface)) {
        log_msg("  swallowed: popup keyboard leave");
        return;
    }
    if (surface == main_surface && popup_is_open()) {
        log_msg("  swallowed: toplevel keeps focus while a menu is open");
        return;
    }
    if (focused_keys == hook)
        focused_keys = NULL;
    if (orig && orig->leave)
        orig->leave(data, keyboard, serial, surface);
}

static void keyboard_keymap(void *data, struct wl_keyboard *keyboard, uint32_t format,
                            int32_t fd, uint32_t size)
{
    struct hook *hook = hook_find(keyboard_hooks, keyboard);
    const struct wl_keyboard_listener *orig = hook ? hook->listener : NULL;

    if (orig && orig->keymap)
        orig->keymap(data, keyboard, format, fd, size);
}

static void keyboard_key(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                         uint32_t time, uint32_t key, uint32_t state)
{
    struct hook *hook = hook_find(keyboard_hooks, keyboard);
    const struct wl_keyboard_listener *orig = hook ? hook->listener : NULL;

    if (state == WL_KEYBOARD_KEY_STATE_PRESSED)
        log_msg("key %u", key);
    /* 158 Back, 174 Exit. 1198/1199 are the wheel's cursor-show/hide keys. */
    if (key == 158 || key == 174 || key == 1198 || key == 1199) {
        if (key == 158 && state == WL_KEYBOARD_KEY_STATE_PRESSED)
            hide_keyboard();
        return;
    }
    if (orig && orig->key)
        orig->key(data, keyboard, serial, time, key, state);
}

static void keyboard_modifiers(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                               uint32_t depressed, uint32_t latched, uint32_t locked,
                               uint32_t group)
{
    struct hook *hook = hook_find(keyboard_hooks, keyboard);
    const struct wl_keyboard_listener *orig = hook ? hook->listener : NULL;

    if (orig && orig->modifiers)
        orig->modifiers(data, keyboard, serial, depressed, latched, locked, group);
}

static void keyboard_repeat(void *data, struct wl_keyboard *keyboard, int32_t rate,
                            int32_t delay)
{
    struct hook *hook = hook_find(keyboard_hooks, keyboard);
    const struct wl_keyboard_listener *orig = hook ? hook->listener : NULL;

    if (orig && orig->repeat_info)
        orig->repeat_info(data, keyboard, rate, delay);
}

static const struct wl_keyboard_listener keyboard_wrapper = {
    keyboard_keymap,
    keyboard_enter,
    keyboard_leave,
    keyboard_key,
    keyboard_modifiers,
    keyboard_repeat,
};

/* Writes "library+0xoffset" (or "[heap]+0x..", or "unmapped") for addr. */
static void where_is(const void *addr, char *out, size_t n)
{
    char line[512];
    FILE *maps = fopen("/proc/self/maps", "r");

    snprintf(out, n, "unmapped");
    if (!maps)
        return;
    while (fgets(line, sizeof line, maps)) {
        unsigned long lo, hi, pgoff;
        char path[256] = "[anon]";
        const char *base;

        if (sscanf(line, "%lx-%lx %*s %lx %*s %*s %255s", &lo, &hi, &pgoff, path) >= 3 &&
            (unsigned long)addr >= lo && (unsigned long)addr < hi) {
            base = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
            snprintf(out, n, "%s+0x%lx", base, (unsigned long)addr - lo + pgoff);
            break;
        }
    }
    fclose(maps);
}

/* True if n bytes at p can be read: read_own_memory() fails with EFAULT on
 * unmapped and on reserved (PROT_NONE) memory, where mincore would not. */
static int is_readable(const void *p, size_t n)
{
    char buf[64];
    ssize_t r;

    if (!p)
        return 0;
    if (n > sizeof buf)
        n = sizeof buf;
    r = read_own_memory(buf, p, n);
    return r == (ssize_t)n || (r < 0 && errno != EFAULT);
}

/* Interface name for a report, without trusting a possibly freed proxy. */
static const char *proxy_name(struct wl_proxy *proxy)
{
    const struct wl_interface *iface = proxy_iface(proxy);

    if (iface && is_readable(iface, sizeof *iface) && is_readable(iface->name, 1))
        return iface->name;
    return "?";
}

void *webos_xdg_last_caller(void);

void webos_xdg_prepare_listener(struct wl_proxy *proxy, void (***implementation)(void), void **data)
{
    const struct wl_interface *iface = proxy_iface(proxy);
    const char *name;
    struct virt *v;

    if (iface && (!is_readable(iface, sizeof *iface) || !is_readable(iface->name, 1))) {
        char at[300], by[300];

        where_is(iface, at, sizeof at);
        where_is(webos_xdg_last_caller(), by, sizeof by);
        log_msg("listener on proxy %p id %u: interface %p (%s) is not a wl_interface, added by %s",
                (void *)proxy, wl_proxy_get_id(proxy), (void *)iface, at, by);
        return;
    }
    name = iface ? iface->name : "";
    v = virt_get(proxy);
    log_msg("listener %s", name);
    if (!strcmp(name, "wl_pointer") || !strcmp(name, "wl_keyboard")) {
        struct hook *hook = hook_slot(!strcmp(name, "wl_pointer") ? pointer_hooks : keyboard_hooks,
                                      proxy);
        if (hook) {
            hook->proxy = proxy;
            hook->listener = *implementation;
            hook->data = *data;
            if (!strcmp(name, "wl_keyboard") && !focused_keys)
                focused_keys = hook;
            *implementation = (void (**)(void))(!strcmp(name, "wl_pointer")
                                                    ? (void *)&pointer_wrapper
                                                    : (void *)&keyboard_wrapper);
        }
    }
    if (v && v->kind != 0) {
        v->listener = *implementation;
        v->data = *data;
        return;
    }
    if (strcmp(name, "wl_registry") != 0)
        return;
    /* GDK's registry. Mali's EGL makes its own on a private queue and
     * destroys that queue; objects bound through it would be orphaned. */
    if (!g.registry && webos_xdg_on_default_queue(proxy))
        g.registry = (struct wl_registry *)proxy;
    if (!v)
        v = virt_add(proxy, 0);
    if (!v)
        return;
    v->listener = *implementation;
    v->data = *data;
    *implementation = (void (**)(void))&registry_wrapper;
}

void webos_xdg_listener_ready(struct wl_proxy *proxy)
{
    struct virt *v = virt_get(proxy);
    if (v && v->kind == V_TOPLEVEL)
        emit_toplevel_configure(v);
    if (v && v->kind == V_POPUP)
        emit_popup_configure(v);
    if (v && v->kind == V_TEXT_INPUT)
        text_input_enter_main();
}

static int cursor_surface_slot(struct wl_proxy *surface)
{
    int i;

    for (i = 0; i < MAX_CURSOR; i++) {
        if (cursor_surfaces[i] == surface)
            return i;
    }
    return -1;
}

static void cursor_surface_add(struct wl_proxy *surface)
{
    int i;

    if (!surface || cursor_surface_slot(surface) >= 0)
        return;
    for (i = 0; i < MAX_CURSOR; i++) {
        if (!cursor_surfaces[i]) {
            cursor_surfaces[i] = surface;
            log_msg("cursor surface %p: its content stays client-side", (void *)surface);
            return;
        }
    }
    log_msg("cursor surface table full");
}

/* 1: the request must not reach the compositor. */
static int cursor_surface_request(struct wl_proxy *surface, uint32_t opcode)
{
    static int dropped;
    int slot = cursor_surface_slot(surface);

    if (slot < 0)
        return 0;
    switch (opcode) {
    case WL_SURFACE_DESTROY:
        cursor_surfaces[slot] = NULL;
        return 0;
    case WL_SURFACE_ATTACH:
    case WL_SURFACE_DAMAGE:
    case WL_SURFACE_SET_OPAQUE_REGION:
    case WL_SURFACE_SET_INPUT_REGION:
    case WL_SURFACE_COMMIT:
    case WL_SURFACE_SET_BUFFER_TRANSFORM:
    case WL_SURFACE_SET_BUFFER_SCALE:
    case WL_SURFACE_DAMAGE_BUFFER:
    case WL_SURFACE_OFFSET:
        if (dropped < 8) {
            dropped++;
            log_msg("dropped cursor surface request %u", opcode);
        }
        return 1;
    default:
        return 0;
    }
}

/* Our libwayland calls this when asked to wrap a proxy whose event queue
 * has been destroyed (queue == NULL), before substituting the default queue. */
void webos_xdg_null_queue_report(struct wl_proxy *proxy, void *caller)
{
    static int reports;
    char by[300];

    if (reports++ >= 12)
        return;
    where_is(caller, by, sizeof by);
    log_msg("wrapping %s@%u whose queue was destroyed, for %s (default queue used)",
            proxy_name(proxy), wl_proxy_get_id(proxy), by);
}

/* libwayland aborts when wl_proxy_destroy() is called on a wrapper proxy.
 * Our patched libwayland frees the wrapper instead and calls this, with the
 * return address recorded at the public entry point, so the offending
 * library can be found: resolve the offset with nm/addr2line on that file. */
void webos_xdg_wrapper_destroy_report(struct wl_proxy *proxy, void *caller)
{
    static int reports;
    char by[300];

    if (reports++ >= 12)
        return;
    where_is(caller, by, sizeof by);
    log_msg("WRAPPER destroyed with wl_proxy_destroy: %s, caller %s (recovered)",
            proxy_name(proxy), by);
}

int webos_xdg_intercept(struct wl_proxy *proxy, uint32_t opcode,
                        const struct wl_interface *interface, uint32_t version,
                        uint32_t flags, union wl_argument *args, struct wl_proxy **out)
{
    const char *name = proxy_iface(proxy) ? proxy_iface(proxy)->name : "";
    struct virt *v;

    if (!strcmp(name, "wl_display")) {
        /* The proxy may be a wrapper that Mali's EGL made for its private
         * queue and frees soon after; keep the display itself. */
        g.display = webos_xdg_proxy_display(proxy);
        wake_display = g.display;
        /* Queued remote events are applied by remote_sync_done, on the
         * thread that dispatches the display. Draining here ran GDK
         * handlers on the evdev reader thread. */
    }

    if (!strcmp(name, "wl_registry") && opcode == WL_REGISTRY_BIND && interface && args) {
        if (!strcmp(interface->name, "zwp_text_input_manager_v3")) {
            *out = new_virt(proxy, interface, version, V_TEXT_INPUT_MANAGER, NULL);
            text_input_bound = 1;
            log_msg("bound virtual zwp_text_input_manager_v3");
            return 1;
        }
        if (!strcmp(interface->name, "xdg_wm_base")) {
            struct wl_proxy *created = new_virt(proxy, interface, version, V_WM, NULL);
            log_msg("bound virtual xdg_wm_base version %u", version);
            bind_named();
            *out = created;
            return 1;
        }
        if (!strcmp(interface->name, "wl_compositor") && server_compositor_version &&
            args[2].u > server_compositor_version) {
            log_msg("clamp compositor %u -> %u", args[2].u, server_compositor_version);
            args[2].u = server_compositor_version;
        }
        if (!strcmp(interface->name, "wl_output") && server_output_version &&
            args[2].u > server_output_version) {
            log_msg("clamp output %u -> %u", args[2].u, server_output_version);
            args[2].u = server_output_version;
        }
        return 0;
    }

    /* Every subsurface, whether ours for a popup or Firefox's own
     * MozContainer, must be tagged before it ever carries content. */
    if (!strcmp(name, "wl_subcompositor") && opcode == WL_SUBCOMPOSITOR_GET_SUBSURFACE &&
        args) {
        tag_child_surface((struct wl_proxy *)args[1].o);
        return 0;
    }

    /* The TV draws the Magic Remote pointer. Remember the surface GDK
     * uses for its cursor and keep it empty: with content on it LSM
     * would map a nameless card and minimize the browser window. */
    if (!strcmp(name, "wl_pointer") && opcode == WL_POINTER_SET_CURSOR) {
        if (args)
            cursor_surface_add((struct wl_proxy *)args[1].o);
        *out = NULL;
        return 1;
    }
    if (!strcmp(name, "wl_surface") && cursor_surface_request(proxy, opcode)) {
        *out = NULL;
        return 1;
    }

    /* wl_surface v4 request. The TV compositor is v3, so send the
     * surface-coordinate damage request instead. At scale 1 they match.
     */
    if (!strcmp(name, "wl_surface") && opcode == WL_SURFACE_DAMAGE_BUFFER &&
        server_compositor_version && server_compositor_version < 4 && args) {
        static int once;
        if (!once) {
            once = 1;
            log_msg("translate damage_buffer");
        }
        wl_proxy_marshal(proxy, WL_SURFACE_DAMAGE,
                         args[0].i, args[1].i, args[2].i, args[3].i);
        *out = NULL;
        return 1;
    }

    if (!strcmp(name, "wl_output") && opcode == WL_OUTPUT_RELEASE &&
        server_output_version && server_output_version < 3) {
        wl_proxy_destroy(proxy);
        *out = NULL;
        return 1;
    }

    v = virt_get(proxy);
    if (!v || v->kind == 0)
        return 0;
    log_msg("virt kind %d opcode %u", v->kind, opcode);
    *out = virt_request(v, opcode, flags, interface, args);
    return 1;
}
