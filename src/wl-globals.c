#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <wayland-client.h>

static void on_global(void *data, struct wl_registry *registry, uint32_t id,
                      const char *interface, uint32_t version)
{
    (void)data;
    (void)registry;
    printf("%s v%u\n", interface, version);
}

static void on_remove(void *data, struct wl_registry *registry, uint32_t id)
{
    (void)data;
    (void)registry;
    (void)id;
}

static const struct wl_registry_listener listener = {on_global, on_remove};

int main(void)
{
    struct wl_display *display = wl_display_connect(NULL);
    if (!display) {
        perror("wl_display_connect");
        return 1;
    }
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &listener, NULL);
    wl_display_roundtrip(display);
    wl_display_disconnect(display);
    return 0;
}
