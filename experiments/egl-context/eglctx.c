/* Which eglCreateContext attribute sets does the TV's Mali driver accept?
 * Mirrors the combinations Firefox's GLContextEGL::CreateGLContext tries,
 * each with a real config and with EGL_NO_CONFIG_KHR. No window is created. */
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
typedef void *P; typedef int I;
#define NONE 0x3038
static P (*getPlatformDisplay)(unsigned, P, const long *);
static P (*getDisplay)(P); static unsigned (*initialize)(P, I *, I *);
static const char *(*query)(P, I); static unsigned (*bindAPI)(unsigned);
static unsigned (*chooseConfig)(P, const I *, P *, I, I *);
static P (*createContext)(P, P, P, const I *); static unsigned (*destroyContext)(P, P);
static I (*getError)(void);
static P dpy;
static void try_it(const char *name, P cfg, const I *attrs) {
    P ctx = createContext(dpy, cfg, 0, attrs);
    I err = ctx ? 0x3000 : getError();
    printf("  %-44s %-9s %s\n", name, cfg ? "config" : "NO_CONFIG", ctx ? "OK" : (err == 0x300C ? "EGL_BAD_PARAMETER" : err == 0x3004 ? "EGL_BAD_ATTRIBUTE" : err == 0x3005 ? "EGL_BAD_CONFIG" : "other error"));
    if (!ctx) printf("%*s(0x%x)\n", 58, "", err);
    if (ctx) destroyContext(dpy, ctx);
}
int main(void) {
    void *wl = dlopen("libwayland-client.so.0", RTLD_NOW | RTLD_GLOBAL);
    void *egl = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
    P (*connect)(const char *) = dlsym(wl, "wl_display_connect");
    getPlatformDisplay = dlsym(egl, "eglGetPlatformDisplayEXT"); getDisplay = dlsym(egl, "eglGetDisplay");
    initialize = dlsym(egl, "eglInitialize"); query = dlsym(egl, "eglQueryString"); bindAPI = dlsym(egl, "eglBindAPI");
    chooseConfig = dlsym(egl, "eglChooseConfig"); createContext = dlsym(egl, "eglCreateContext");
    destroyContext = dlsym(egl, "eglDestroyContext"); getError = dlsym(egl, "eglGetError");
    P wd = connect(0);
    dpy = getPlatformDisplay ? getPlatformDisplay(0x31D8, wd, 0) : getDisplay(wd);
    I ma, mi; if (!initialize(dpy, &ma, &mi)) { printf("eglInitialize failed\n"); return 1; }
    const char *ext = query(dpy, 0x3055);
    printf("EGL %d.%d  %s\n", ma, mi, query(dpy, 0x3054));
    const char *want[] = {"EGL_KHR_no_config_context", "EGL_KHR_create_context", "EGL_KHR_create_context_no_error",
                          "EGL_EXT_create_context_robustness", "EGL_KHR_surfaceless_context", "EGL_EXT_device_query", 0};
    for (int i = 0; want[i]; i++) printf("  %-36s %s\n", want[i], strstr(ext, want[i]) ? "yes" : "no");
    bindAPI(0x30A0);
    const I ca[] = {0x3033, 0x0004 | 0x0001, 0x3040, 0x0040, 0x3024, 8, 0x3023, 8, 0x3022, 8, 0x3021, 8, NONE};
    P cfg = 0; I n = 0; chooseConfig(dpy, ca, &cfg, 1, &n);
    printf("config found: %s\n", n ? "yes" : "no");
    const I plain[] = {0x3098, 3, NONE, 0, 0, 0};
    const I noerr[] = {0x3098, 3, 0x31B3, 1, NONE, 0, 0, 0};
    const I khr[] = {0x3098, 3, 0x31BD, 0x31BF, NONE, 0, 0, 0};
    const I khrrb[] = {0x3098, 3, 0x31BD, 0x31BF, 0x30FC, 0x4, NONE, 0, 0, 0};
    const I extr[] = {0x3098, 3, 0x3138, 0x31BF, NONE, 0, 0, 0};
    const I extrb[] = {0x3098, 3, 0x3138, 0x31BF, 0x30BF, 1, NONE, 0, 0, 0};
    const I es2[] = {0x3098, 2, NONE, 0, 0, 0};
    const struct { const char *n; const I *a; } sets[] = {
        {"ES3 plain (required_attribs)", plain}, {"ES3 + NO_ERROR_KHR", noerr},
        {"ES3 + KHR reset strategy", khr}, {"ES3 + KHR reset + robust access (khr_rbab)", khrrb},
        {"ES3 + EXT reset strategy", extr}, {"ES3 + EXT reset + robust access (ext_rbab)", extrb},
        {"ES2 plain", es2}, {0, 0}};
    for (int i = 0; sets[i].n; i++) { try_it(sets[i].n, cfg, sets[i].a); try_it(sets[i].n, 0, sets[i].a); }
    return 0;
}
