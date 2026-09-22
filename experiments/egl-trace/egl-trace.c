/* Tracing libEGL.so.1 for Firefox on the TV. Placed in firefox-runtime, it is
 * found before /usr/lib/libEGL.so.1 through LD_LIBRARY_PATH, logs the calls
 * that decide whether GPU rendering starts, and forwards everything to the
 * TV's library. Errors read here are handed back to the caller's own
 * eglGetError(), so Firefox sees the same codes. Firefox opens libEGL.so
 * before libEGL.so.1, so install it under both names:
 *   cp out/libEGL.so.1 <runtime>/ && ln -s libEGL.so.1 <runtime>/libEGL.so
 * and remove both after tracing. */
#define _GNU_SOURCE
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#define REAL_LIB "/usr/lib/libEGL.so.1.4.0"

static void *real_lib;
static __thread EGLint pending_error;

static void *real(const char *name)
{
    if (!real_lib)
        real_lib = dlopen(REAL_LIB, RTLD_NOW | RTLD_GLOBAL);
    return real_lib ? dlsym(real_lib, name) : NULL;
}

#define FWD(ret, name, params, args) \
    EGLAPI ret EGLAPIENTRY name params \
    { \
        static ret (*fn) params; \
        if (!fn) \
            fn = (ret (*) params)real(#name); \
        return fn args; \
    }

static void trace(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void trace(const char *fmt, ...)
{
    char buf[1024];
    int n = snprintf(buf, sizeof buf, "egl-trace[%ld]: ", (long)syscall(SYS_gettid));
    va_list ap;

    va_start(ap, fmt);
    n += vsnprintf(buf + n, sizeof buf - n, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof buf - 2)
        n = sizeof buf - 2;
    buf[n++] = '\n';
    write(STDERR_FILENO, buf, n);
}

static const char *attrs_str(const EGLint *a, char *buf, size_t n)
{
    size_t used = 0;

    buf[0] = '\0';
    for (; a && *a != EGL_NONE && used + 24 < n; a += 2)
        used += snprintf(buf + used, n - used, "%#x=%#x ", a[0], a[1]);
    return buf;
}

static EGLint note_error(void)
{
    static EGLint (*get)(void);

    if (!get)
        get = (EGLint (*)(void))real("eglGetError");
    pending_error = get();
    return pending_error;
}

EGLAPI EGLint EGLAPIENTRY eglGetError(void)
{
    static EGLint (*get)(void);
    EGLint e;

    if (pending_error) {
        e = pending_error;
        pending_error = 0;
        return e;
    }
    if (!get)
        get = (EGLint (*)(void))real("eglGetError");
    return get();
}

EGLAPI EGLDisplay EGLAPIENTRY eglGetDisplay(EGLNativeDisplayType d)
{
    static EGLDisplay (*fn)(EGLNativeDisplayType);
    EGLDisplay r;

    if (!fn)
        fn = (EGLDisplay (*)(EGLNativeDisplayType))real("eglGetDisplay");
    r = fn(d);
    trace("eglGetDisplay(%p) = %p", (void *)d, r);
    return r;
}

static EGLDisplay EGLAPIENTRY traced_GetPlatformDisplay(EGLenum platform, void *d, const EGLAttrib *attr)
{
    static EGLDisplay (*fn)(EGLenum, void *, const EGLAttrib *);
    EGLDisplay r;

    if (!fn)
        fn = (EGLDisplay (*)(EGLenum, void *, const EGLAttrib *))real("eglGetPlatformDisplay");
    r = fn(platform, d, attr);
    trace("eglGetPlatformDisplay(%#x, %p) = %p", platform, d, r);
    if (!r)
        note_error();
    return r;
}

EGLAPI EGLBoolean EGLAPIENTRY eglInitialize(EGLDisplay d, EGLint *major, EGLint *minor)
{
    static EGLBoolean (*fn)(EGLDisplay, EGLint *, EGLint *);
    EGLBoolean r;

    if (!fn)
        fn = (EGLBoolean (*)(EGLDisplay, EGLint *, EGLint *))real("eglInitialize");
    r = fn(d, major, minor);
    trace("eglInitialize(%p) = %d %d.%d err %#x", d, r, major ? *major : -1, minor ? *minor : -1,
          r ? 0 : note_error());
    return r;
}

/* EGL_TRACE_SKIP_TERMINATE=1 keeps displays alive, to test whether a
 * failure depends on terminating and re-initializing a display. */
EGLAPI EGLBoolean EGLAPIENTRY eglTerminate(EGLDisplay d)
{
    static EGLBoolean (*fn)(EGLDisplay);

    if (getenv("EGL_TRACE_SKIP_TERMINATE")) {
        trace("eglTerminate(%p) skipped", d);
        return EGL_TRUE;
    }
    if (!fn)
        fn = (EGLBoolean (*)(EGLDisplay))real("eglTerminate");
    trace("eglTerminate(%p)", d);
    return fn(d);
}

EGLAPI EGLContext EGLAPIENTRY eglCreateContext(EGLDisplay d, EGLConfig c, EGLContext share,
                                               const EGLint *attrs)
{
    static EGLContext (*fn)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
    char buf[512];
    EGLContext r;

    if (!fn)
        fn = (EGLContext (*)(EGLDisplay, EGLConfig, EGLContext, const EGLint *))real("eglCreateContext");
    r = fn(d, c, share, attrs);
    trace("eglCreateContext(%p, config %p, share %p, [%s]) = %p err %#x", d, c, share,
          attrs_str(attrs, buf, sizeof buf), r, r ? 0 : note_error());
    return r;
}

EGLAPI EGLBoolean EGLAPIENTRY eglMakeCurrent(EGLDisplay d, EGLSurface draw, EGLSurface read, EGLContext ctx)
{
    static EGLBoolean (*fn)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
    static int logged;
    EGLBoolean r;

    if (!fn)
        fn = (EGLBoolean (*)(EGLDisplay, EGLSurface, EGLSurface, EGLContext))real("eglMakeCurrent");
    r = fn(d, draw, read, ctx);
    if (!r || logged++ < 20)
        trace("eglMakeCurrent(%p, %p, %p, %p) = %d err %#x", d, draw, read, ctx, r,
              r ? 0 : note_error());
    return r;
}

EGLAPI EGLSurface EGLAPIENTRY eglCreateWindowSurface(EGLDisplay d, EGLConfig c, EGLNativeWindowType w,
                                                     const EGLint *attrs)
{
    static EGLSurface (*fn)(EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint *);
    char buf[256];
    EGLSurface r;

    if (!fn)
        fn = (EGLSurface (*)(EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint *))
            real("eglCreateWindowSurface");
    r = fn(d, c, w, attrs);
    trace("eglCreateWindowSurface(%p, %p, win %p, [%s]) = %p err %#x", d, c, (void *)w,
          attrs_str(attrs, buf, sizeof buf), r, r ? 0 : note_error());
    return r;
}

EGLAPI EGLSurface EGLAPIENTRY eglCreatePbufferSurface(EGLDisplay d, EGLConfig c, const EGLint *attrs)
{
    static EGLSurface (*fn)(EGLDisplay, EGLConfig, const EGLint *);
    char buf[256];
    EGLSurface r;

    if (!fn)
        fn = (EGLSurface (*)(EGLDisplay, EGLConfig, const EGLint *))real("eglCreatePbufferSurface");
    r = fn(d, c, attrs);
    trace("eglCreatePbufferSurface(%p, %p, [%s]) = %p err %#x", d, c,
          attrs_str(attrs, buf, sizeof buf), r, r ? 0 : note_error());
    return r;
}

EGLAPI EGLBoolean EGLAPIENTRY eglChooseConfig(EGLDisplay d, const EGLint *attrs, EGLConfig *configs,
                                              EGLint size, EGLint *num)
{
    static EGLBoolean (*fn)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
    char buf[512];
    EGLBoolean r;

    if (!fn)
        fn = (EGLBoolean (*)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *))
            real("eglChooseConfig");
    r = fn(d, attrs, configs, size, num);
    trace("eglChooseConfig(%p, [%s]) = %d, %d configs, first %p err %#x", d,
          attrs_str(attrs, buf, sizeof buf), r, num ? *num : -1,
          configs && num && *num > 0 ? configs[0] : NULL, r ? 0 : note_error());
    return r;
}

EGLAPI const char *EGLAPIENTRY eglQueryString(EGLDisplay d, EGLint name)
{
    static const char *(*fn)(EGLDisplay, EGLint);
    const char *r;

    if (!fn)
        fn = (const char *(*)(EGLDisplay, EGLint))real("eglQueryString");
    r = fn(d, name);
    if (name == EGL_EXTENSIONS)
        trace("eglQueryString(%p, EXTENSIONS) = %s", d, r ? r : "(null)");
    return r;
}

EGLAPI __eglMustCastToProperFunctionPointerType EGLAPIENTRY eglGetProcAddress(const char *name)
{
    static __eglMustCastToProperFunctionPointerType (*fn)(const char *);

    if (!strcmp(name, "eglGetPlatformDisplay") || !strcmp(name, "eglGetPlatformDisplayEXT"))
        return (__eglMustCastToProperFunctionPointerType)traced_GetPlatformDisplay;
    if (!fn)
        fn = (__eglMustCastToProperFunctionPointerType (*)(const char *))real("eglGetProcAddress");
    return fn(name);
}

FWD(EGLSurface, eglGetCurrentSurface, (EGLint w), (w))
FWD(EGLContext, eglGetCurrentContext, (void), ())
FWD(EGLBoolean, eglDestroyContext, (EGLDisplay d, EGLContext c), (d, c))
FWD(EGLBoolean, eglDestroySurface, (EGLDisplay d, EGLSurface s), (d, s))
FWD(EGLSurface, eglCreatePbufferFromClientBuffer,
    (EGLDisplay d, EGLenum t, EGLClientBuffer b, EGLConfig c, const EGLint *a), (d, t, b, c, a))
FWD(EGLSurface, eglCreatePixmapSurface,
    (EGLDisplay d, EGLConfig c, EGLNativePixmapType p, const EGLint *a), (d, c, p, a))
FWD(EGLBoolean, eglBindAPI, (EGLenum api), (api))
FWD(EGLBoolean, eglGetConfigs, (EGLDisplay d, EGLConfig *c, EGLint s, EGLint *n), (d, c, s, n))
FWD(EGLBoolean, eglGetConfigAttrib, (EGLDisplay d, EGLConfig c, EGLint a, EGLint *v), (d, c, a, v))
FWD(EGLBoolean, eglWaitNative, (EGLint e), (e))
FWD(EGLBoolean, eglSwapBuffers, (EGLDisplay d, EGLSurface s), (d, s))
FWD(EGLBoolean, eglCopyBuffers, (EGLDisplay d, EGLSurface s, EGLNativePixmapType t), (d, s, t))
FWD(EGLBoolean, eglQueryContext, (EGLDisplay d, EGLContext c, EGLint a, EGLint *v), (d, c, a, v))
FWD(EGLBoolean, eglBindTexImage, (EGLDisplay d, EGLSurface s, EGLint b), (d, s, b))
FWD(EGLBoolean, eglReleaseTexImage, (EGLDisplay d, EGLSurface s, EGLint b), (d, s, b))
FWD(EGLBoolean, eglSwapInterval, (EGLDisplay d, EGLint i), (d, i))
FWD(EGLBoolean, eglQuerySurface, (EGLDisplay d, EGLSurface s, EGLint a, EGLint *v), (d, s, a, v))
FWD(EGLBoolean, eglWaitClient, (void), ())
FWD(EGLBoolean, eglWaitGL, (void), ())
FWD(EGLBoolean, eglReleaseThread, (void), ())
FWD(EGLenum, eglQueryAPI, (void), ())
FWD(EGLBoolean, eglSurfaceAttrib, (EGLDisplay d, EGLSurface s, EGLint a, EGLint v), (d, s, a, v))
FWD(EGLDisplay, eglGetCurrentDisplay, (void), ())
