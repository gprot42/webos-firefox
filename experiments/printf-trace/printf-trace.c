/* LD_PRELOAD diagnostic: remembers, per thread, the caller and format of the
 * last printf-family call. When a format argument is bad and printf faults,
 * the webos-xdg crash handler prints this record, naming the call site in a
 * library that has no symbols. Enable with a line in <app dir>/env:
 *   LD_PRELOAD=<app dir>/firefox-runtime/libprintf-trace.so
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>

struct printf_trace {
    void *caller;
    const char *fmt;
    const char *fn;
};

static __thread struct printf_trace last;

struct printf_trace *webos_printf_trace_get(void);
struct printf_trace *webos_printf_trace_get(void)
{
    return &last;
}

#define REAL(name, type) \
    static type real_##name; \
    if (!real_##name) \
        real_##name = (type)dlsym(RTLD_NEXT, #name)

#define NOTE(name, f) \
    (last.caller = __builtin_return_address(0), last.fmt = (f), last.fn = #name)

typedef int (*vfprintf_t)(FILE *, const char *, va_list);
typedef int (*vsnprintf_t)(char *, size_t, const char *, va_list);
typedef int (*vsprintf_t)(char *, const char *, va_list);
typedef int (*vasprintf_t)(char **, const char *, va_list);
typedef int (*vdprintf_t)(int, const char *, va_list);
typedef int (*vfprintf_chk_t)(FILE *, int, const char *, va_list);
typedef int (*vsnprintf_chk_t)(char *, size_t, int, size_t, const char *, va_list);

int vfprintf(FILE *fp, const char *fmt, va_list ap)
{
    REAL(vfprintf, vfprintf_t);
    NOTE(vfprintf, fmt);
    return real_vfprintf(fp, fmt, ap);
}

int vprintf(const char *fmt, va_list ap)
{
    REAL(vfprintf, vfprintf_t);
    NOTE(vprintf, fmt);
    return real_vfprintf(stdout, fmt, ap);
}

int fprintf(FILE *fp, const char *fmt, ...)
{
    va_list ap;
    int r;
    REAL(vfprintf, vfprintf_t);
    NOTE(fprintf, fmt);
    va_start(ap, fmt);
    r = real_vfprintf(fp, fmt, ap);
    va_end(ap);
    return r;
}

int printf(const char *fmt, ...)
{
    va_list ap;
    int r;
    REAL(vfprintf, vfprintf_t);
    NOTE(printf, fmt);
    va_start(ap, fmt);
    r = real_vfprintf(stdout, fmt, ap);
    va_end(ap);
    return r;
}

int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap)
{
    REAL(vsnprintf, vsnprintf_t);
    NOTE(vsnprintf, fmt);
    return real_vsnprintf(buf, n, fmt, ap);
}

int snprintf(char *buf, size_t n, const char *fmt, ...)
{
    va_list ap;
    int r;
    REAL(vsnprintf, vsnprintf_t);
    NOTE(snprintf, fmt);
    va_start(ap, fmt);
    r = real_vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}

int vsprintf(char *buf, const char *fmt, va_list ap)
{
    REAL(vsprintf, vsprintf_t);
    NOTE(vsprintf, fmt);
    return real_vsprintf(buf, fmt, ap);
}

int sprintf(char *buf, const char *fmt, ...)
{
    va_list ap;
    int r;
    REAL(vsprintf, vsprintf_t);
    NOTE(sprintf, fmt);
    va_start(ap, fmt);
    r = real_vsprintf(buf, fmt, ap);
    va_end(ap);
    return r;
}

int vasprintf(char **out, const char *fmt, va_list ap)
{
    REAL(vasprintf, vasprintf_t);
    NOTE(vasprintf, fmt);
    return real_vasprintf(out, fmt, ap);
}

int asprintf(char **out, const char *fmt, ...)
{
    va_list ap;
    int r;
    REAL(vasprintf, vasprintf_t);
    NOTE(asprintf, fmt);
    va_start(ap, fmt);
    r = real_vasprintf(out, fmt, ap);
    va_end(ap);
    return r;
}

int vdprintf(int fd, const char *fmt, va_list ap)
{
    REAL(vdprintf, vdprintf_t);
    NOTE(vdprintf, fmt);
    return real_vdprintf(fd, fmt, ap);
}

int dprintf(int fd, const char *fmt, ...)
{
    va_list ap;
    int r;
    REAL(vdprintf, vdprintf_t);
    NOTE(dprintf, fmt);
    va_start(ap, fmt);
    r = real_vdprintf(fd, fmt, ap);
    va_end(ap);
    return r;
}

int __vfprintf_chk(FILE *fp, int flag, const char *fmt, va_list ap)
{
    REAL(__vfprintf_chk, vfprintf_chk_t);
    NOTE(__vfprintf_chk, fmt);
    return real___vfprintf_chk(fp, flag, fmt, ap);
}

int __fprintf_chk(FILE *fp, int flag, const char *fmt, ...)
{
    va_list ap;
    int r;
    REAL(__vfprintf_chk, vfprintf_chk_t);
    NOTE(__fprintf_chk, fmt);
    va_start(ap, fmt);
    r = real___vfprintf_chk(fp, flag, fmt, ap);
    va_end(ap);
    return r;
}

int __printf_chk(int flag, const char *fmt, ...)
{
    va_list ap;
    int r;
    REAL(__vfprintf_chk, vfprintf_chk_t);
    NOTE(__printf_chk, fmt);
    va_start(ap, fmt);
    r = real___vfprintf_chk(stdout, flag, fmt, ap);
    va_end(ap);
    return r;
}

int __vsnprintf_chk(char *buf, size_t n, int flag, size_t slen, const char *fmt, va_list ap)
{
    REAL(__vsnprintf_chk, vsnprintf_chk_t);
    NOTE(__vsnprintf_chk, fmt);
    return real___vsnprintf_chk(buf, n, flag, slen, fmt, ap);
}

int __snprintf_chk(char *buf, size_t n, int flag, size_t slen, const char *fmt, ...)
{
    va_list ap;
    int r;
    REAL(__vsnprintf_chk, vsnprintf_chk_t);
    NOTE(__snprintf_chk, fmt);
    va_start(ap, fmt);
    r = real___vsnprintf_chk(buf, n, flag, slen, fmt, ap);
    va_end(ap);
    return r;
}
