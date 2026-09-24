/* libwayland-egl.so.1 for TVs that have none (webOS 4). GTK cannot load
 * without this library, and Firefox creates its EGL windows through it.
 *
 * Wayland's own libwayland-egl defines struct wl_egl_window (version 3), and
 * newer Mali drivers are built against that. Older Mali drivers, the ones
 * that talk to the compositor over wl_mali as webOS 4's does, implement these
 * functions themselves with their own struct, and a TV without the library
 * suggests exactly that. So hand the calls to the GPU driver when it exports
 * them, and use the generic version 3 implementation only when it does not.
 * The launcher adds this library only on TVs without their own. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

struct wl_surface;

/* Wayland's wayland-egl-backend.h, version 3. */
#define WL_EGL_WINDOW_VERSION 3

struct wl_egl_window {
    const intptr_t version;
    int width;
    int height;
    int dx;
    int dy;
    int attached_width;
    int attached_height;
    void *driver_private;
    void (*resize_callback)(struct wl_egl_window *, void *);
    void (*destroy_window_callback)(void *);
    struct wl_surface *surface;
};

#define EXPORT __attribute__((visibility("default")))

EXPORT struct wl_egl_window *wl_egl_window_create(struct wl_surface *surface, int width,
                                                  int height);
EXPORT void wl_egl_window_destroy(struct wl_egl_window *egl_window);
EXPORT void wl_egl_window_resize(struct wl_egl_window *egl_window, int width, int height,
                                 int dx, int dy);
EXPORT void wl_egl_window_get_attached_size(struct wl_egl_window *egl_window, int *width,
                                            int *height);

static struct {
    int looked;
    struct wl_egl_window *(*create)(struct wl_surface *, int, int);
    void (*destroy)(struct wl_egl_window *);
    void (*resize)(struct wl_egl_window *, int, int, int, int);
    void (*get_attached_size)(struct wl_egl_window *, int *, int *);
} drv;

static void find_driver(void)
{
    static const char *const libs[] = {
        "libEGL.so.1", "libEGL.so", "libmali.so.0", "libmali.so", NULL
    };
    int i;

    if (drv.looked)
        return;
    drv.looked = 1;
    for (i = 0; libs[i]; i++) {
        void *h = dlopen(libs[i], RTLD_LAZY | RTLD_LOCAL);
        void *create;

        if (!h)
            continue;
        /* dlsym on a handle searches that library and what it loads, never
         * this one (the TV has no libwayland-egl for them to load). */
        create = dlsym(h, "wl_egl_window_create");
        if (create && create != (void *)wl_egl_window_create) {
            drv.create = (struct wl_egl_window *(*)(struct wl_surface *, int, int))create;
            drv.destroy = (void (*)(struct wl_egl_window *))dlsym(h, "wl_egl_window_destroy");
            drv.resize = (void (*)(struct wl_egl_window *, int, int, int, int))
                dlsym(h, "wl_egl_window_resize");
            drv.get_attached_size = (void (*)(struct wl_egl_window *, int *, int *))
                dlsym(h, "wl_egl_window_get_attached_size");
            if (drv.destroy && drv.resize) {
                fprintf(stderr, "wayland-egl: using the GPU driver's own, from %s\n", libs[i]);
                return;   /* keep the handle */
            }
            drv.create = NULL;
        }
        dlclose(h);
    }
    fprintf(stderr, "wayland-egl: the GPU driver has none, using the generic one\n");
}

struct wl_egl_window *wl_egl_window_create(struct wl_surface *surface, int width, int height)
{
    struct wl_egl_window *w;

    find_driver();
    if (drv.create)
        return drv.create(surface, width, height);
    if (width <= 0 || height <= 0)
        return NULL;
    w = calloc(1, sizeof *w);
    if (!w)
        return NULL;
    /* The version field is const: the driver may only read it. */
    *(intptr_t *)&w->version = WL_EGL_WINDOW_VERSION;
    w->surface = surface;
    w->width = width;
    w->height = height;
    return w;
}

void wl_egl_window_destroy(struct wl_egl_window *w)
{
    if (drv.create) {
        drv.destroy(w);
        return;
    }
    if (!w)
        return;
    if (w->destroy_window_callback)
        w->destroy_window_callback(w->driver_private);
    free(w);
}

void wl_egl_window_resize(struct wl_egl_window *w, int width, int height, int dx, int dy)
{
    if (drv.create) {
        drv.resize(w, width, height, dx, dy);
        return;
    }
    if (!w || width <= 0 || height <= 0)
        return;
    w->width = width;
    w->height = height;
    w->dx = dx;
    w->dy = dy;
    if (w->resize_callback)
        w->resize_callback(w, w->driver_private);
}

void wl_egl_window_get_attached_size(struct wl_egl_window *w, int *width, int *height)
{
    if (drv.create) {
        if (drv.get_attached_size) {
            drv.get_attached_size(w, width, height);
        } else {
            if (width)
                *width = 0;
            if (height)
                *height = 0;
        }
        return;
    }
    if (!w)
        return;
    if (width)
        *width = w->attached_width;
    if (height)
        *height = w->attached_height;
}
