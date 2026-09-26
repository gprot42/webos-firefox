/* libwayland-egl.so.1 for TVs that have none (webOS 4). GTK cannot load
 * without this library, and Firefox creates its EGL windows through it.
 *
 * struct wl_egl_window is shared between this library and the GPU driver,
 * and its layout changed when libwayland-egl moved from Mesa into Wayland
 * (1.15, 2018): the old layout starts with the wl_surface pointer, the new
 * one (version 3) with a version number, so an old driver reading the new
 * layout takes 3 for the surface pointer and crashes on its first frame.
 * That is what webOS 4's Mali-T820 driver (Midgard r12p0) did with our
 * version 3 fallback (build gpuprobe). A TV without libwayland-egl.so.1 has
 * a driver older than that library, so:
 *   - if the GPU driver implements these functions itself, use its own;
 *   - otherwise use the old layout, or version 3 when WEBOS_WAYLAND_EGL_ABI=3.
 * The launcher adds this library only on TVs without their own. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct wl_surface;
struct wl_egl_window;

/* Mesa's wayland-egl-priv.h, before libwayland-egl moved into Wayland. */
struct egl_window_old {
    struct wl_surface *surface;
    int width;
    int height;
    int dx;
    int dy;
    int attached_width;
    int attached_height;
    void *driver_private;
    void (*resize_callback)(struct wl_egl_window *, void *);
    void (*destroy_window_callback)(void *);
};

/* Wayland's wayland-egl-backend.h, version 3. */
struct egl_window_v3 {
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
    int v3;          /* generic implementation: version 3 layout, else the old one */
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
    const char *abi = getenv("WEBOS_WAYLAND_EGL_ABI");
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
    drv.v3 = abi && !strcmp(abi, "3");
    fprintf(stderr, "wayland-egl: the GPU driver has none, using the generic one (%s layout)\n",
            drv.v3 ? "version 3" : "pre-2018");
}

struct wl_egl_window *wl_egl_window_create(struct wl_surface *surface, int width, int height)
{
    find_driver();
    if (drv.create)
        return drv.create(surface, width, height);
    if (width <= 0 || height <= 0)
        return NULL;
    if (drv.v3) {
        struct egl_window_v3 *w = calloc(1, sizeof *w);

        if (!w)
            return NULL;
        /* The version field is const: the driver may only read it. */
        *(intptr_t *)&w->version = 3;
        w->surface = surface;
        w->width = width;
        w->height = height;
        return (struct wl_egl_window *)w;
    } else {
        struct egl_window_old *w = calloc(1, sizeof *w);

        if (!w)
            return NULL;
        w->surface = surface;
        w->width = width;
        w->height = height;
        return (struct wl_egl_window *)w;
    }
}

void wl_egl_window_destroy(struct wl_egl_window *egl_window)
{
    if (drv.create) {
        drv.destroy(egl_window);
        return;
    }
    if (!egl_window)
        return;
    if (drv.v3) {
        struct egl_window_v3 *w = (struct egl_window_v3 *)egl_window;

        if (w->destroy_window_callback)
            w->destroy_window_callback(w->driver_private);
    } else {
        struct egl_window_old *w = (struct egl_window_old *)egl_window;

        if (w->destroy_window_callback)
            w->destroy_window_callback(w->driver_private);
    }
    free(egl_window);
}

void wl_egl_window_resize(struct wl_egl_window *egl_window, int width, int height, int dx,
                          int dy)
{
    if (drv.create) {
        drv.resize(egl_window, width, height, dx, dy);
        return;
    }
    if (!egl_window || width <= 0 || height <= 0)
        return;
    if (drv.v3) {
        struct egl_window_v3 *w = (struct egl_window_v3 *)egl_window;

        w->width = width;
        w->height = height;
        w->dx = dx;
        w->dy = dy;
        if (w->resize_callback)
            w->resize_callback(egl_window, w->driver_private);
    } else {
        struct egl_window_old *w = (struct egl_window_old *)egl_window;

        w->width = width;
        w->height = height;
        w->dx = dx;
        w->dy = dy;
        if (w->resize_callback)
            w->resize_callback(egl_window, w->driver_private);
    }
}

void wl_egl_window_get_attached_size(struct wl_egl_window *egl_window, int *width,
                                     int *height)
{
    int aw = 0, ah = 0;

    if (drv.create) {
        if (drv.get_attached_size)
            drv.get_attached_size(egl_window, width, height);
        else {
            if (width)
                *width = 0;
            if (height)
                *height = 0;
        }
        return;
    }
    if (egl_window && drv.v3) {
        aw = ((struct egl_window_v3 *)egl_window)->attached_width;
        ah = ((struct egl_window_v3 *)egl_window)->attached_height;
    } else if (egl_window) {
        aw = ((struct egl_window_old *)egl_window)->attached_width;
        ah = ((struct egl_window_old *)egl_window)->attached_height;
    }
    if (width)
        *width = aw;
    if (height)
        *height = ah;
}
