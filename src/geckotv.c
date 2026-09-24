#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Entry point. webOS runs this ELF, which sets up the environment and execs
 * the native 32-bit Firefox in firefox-runtime. */

static void mkdir_p(const char *path)
{
    char buf[PATH_MAX];
    size_t len;
    size_t i;

    snprintf(buf, sizeof buf, "%s", path);
    len = strlen(buf);
    for (i = 1; i < len; i++) {
        if (buf[i] != '/')
            continue;
        buf[i] = '\0';
        mkdir(buf, 0755);
        buf[i] = '/';
    }
    mkdir(buf, 0755);
}

/* A root shell leaves the profile owned by root. The home-screen jail
 * runs as another user and Firefox then refuses the directory. */
static void give_profile(const char *path, uid_t uid, gid_t gid)
{
    DIR *dir;
    struct dirent *ent;
    struct stat st;
    char child[PATH_MAX];

    if (lstat(path, &st) != 0)
        return;
    lchown(path, uid, gid);
    if (!S_ISDIR(st.st_mode))
        return;
    dir = opendir(path);
    if (!dir)
        return;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        snprintf(child, sizeof child, "%s/%s", path, ent->d_name);
        give_profile(child, uid, gid);
    }
    closedir(dir);
}

/* The "target" string of the webOS launch parameters, e.g.
 * {"target":"https://example.org/"}. Only http(s) URLs are accepted; JSON
 * escapes are not decoded, and a value containing any is rejected. */
static int launch_target(const char *json, char *out, size_t n)
{
    const char *p = strstr(json, "\"target\"");
    size_t len = 0;

    if (!p)
        return 0;
    p += strlen("\"target\"");
    while (*p == ' ' || *p == ':')
        p++;
    if (*p++ != '"')
        return 0;
    while (p[len] && p[len] != '"' && p[len] != '\\')
        len++;
    if (p[len] != '"' || len == 0 || len >= n)
        return 0;
    if (strncmp(p, "http://", 7) != 0 && strncmp(p, "https://", 8) != 0)
        return 0;
    memcpy(out, p, len);
    out[len] = '\0';
    return 1;
}

int main(int argc, char **argv)
{
    char exe[PATH_MAX];
    char dir[PATH_MAX];
    char home[PATH_MAX];
    char firefox[PATH_MAX];
    char libpath[PATH_MAX * 2];
    char logpath[PATH_MAX];
    char marker[PATH_MAX];
    char target[2048];
    char *args[12];
    int fd;
    int narg = 0;
    ssize_t n;
    char *slash;

    n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n < 0)
        return 1;
    exe[n] = '\0';
    snprintf(dir, sizeof dir, "%s", exe);
    slash = strrchr(dir, '/');
    if (!slash)
        return 1;
    *slash = '\0';

    snprintf(logpath, sizeof logpath, "%s/geckotv.log", dir);
    fd = open(logpath, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        dup2(fd, 1);
        dup2(fd, 2);
        if (fd > 2)
            close(fd);
    }

    snprintf(home, sizeof home, "%s/profile", dir);
    mkdir_p(home);
    mkdir_p("/tmp/xdg");
    if (geteuid() == 0) {
        struct stat jail;
        if (stat("/var/palm/jail/com.github.gprot42.geckotv", &jail) == 0 &&
            jail.st_uid != 0)
            give_profile(home, jail.st_uid, jail.st_gid);
    }
    /* User prefs beat the browser's own default-pref files, which is the
     * only way to override a value that browser/omni.ja also sets, such as
     * the extension process. Written once; the user can edit it after. */
    snprintf(marker, sizeof marker, "%s/user.js", home);
    if (access(marker, F_OK) != 0) {
        FILE *uf = fopen(marker, "w");
        if (uf) {
            fputs("// Written by geckotv on first run. Values here override defaults/pref.\n"
                  "user_pref(\"extensions.webextensions.remote\", false);\n", uf);
            fclose(uf);
        }
    }
    snprintf(libpath, sizeof libpath, "%s/.parentlock", home);
    unlink(libpath);
    snprintf(libpath, sizeof libpath, "%s/parent.lock", home);
    unlink(libpath);
    snprintf(firefox, sizeof firefox, "%s/firefox-runtime/firefox", dir);
    /* The runtime's own libraries come first; its copy of libwayland-client
     * carries the webOS shell adapter. Everything else is the TV's. */
    snprintf(libpath, sizeof libpath, "%s/firefox-runtime", dir);
    /* GTK needs libwayland-egl.so.1 to load. Newer webOS has it, matched to
     * its Mali driver, and must keep using its own; webOS 4 has none, so
     * then use the generic one bundled in firefox-runtime/fallback. */
    if (access("/usr/lib/libwayland-egl.so.1", F_OK) != 0 &&
        access("/lib/libwayland-egl.so.1", F_OK) != 0) {
        size_t len = strlen(libpath);
        snprintf(libpath + len, sizeof libpath - len, ":%s/firefox-runtime/fallback", dir);
        fprintf(stderr, "geckotv: no libwayland-egl.so.1 on this TV, using the bundled one\n");
    }
    /* The runtime's GTK finds its image decoders through this cache. */
    snprintf(marker, sizeof marker, "%s/firefox-runtime/gdk-pixbuf/loaders.cache", dir);
    if (access(marker, R_OK) == 0)
        setenv("GDK_PIXBUF_MODULE_FILE", marker, 1);
    /* GTK's Wayland input-method module: it tells the adapter when an
     * editable field gains or loses focus, which opens and closes the webOS
     * keyboard for text fields in pages, not only the address bar. */
    snprintf(marker, sizeof marker, "%s/firefox-runtime/gtk-immodules/immodules.cache", dir);
    if (access(marker, R_OK) == 0)
        setenv("GTK_IM_MODULE_FILE", marker, 1);
    /* GLib data bundled with the runtime rather than taken from the TV, whose
     * copies may be missing or built for another GLib: compiled GSettings
     * schemas (GTK aborts on a missing one), no GIO plug-ins, settings kept
     * in memory, and keyboard-map data for xkbcommon's fallback keymap. */
    snprintf(marker, sizeof marker, "%s/firefox-runtime/glib-schemas/gschemas.compiled", dir);
    if (access(marker, R_OK) == 0) {
        snprintf(marker, sizeof marker, "%s/firefox-runtime/glib-schemas", dir);
        setenv("GSETTINGS_SCHEMA_DIR", marker, 1);
        setenv("GSETTINGS_BACKEND", "memory", 1);
    }
    snprintf(marker, sizeof marker, "%s/firefox-runtime/gio-modules", dir);
    if (access(marker, R_OK) == 0)
        setenv("GIO_MODULE_DIR", marker, 1);
    snprintf(marker, sizeof marker, "%s/firefox-runtime/xkb/rules", dir);
    if (access(marker, R_OK) == 0) {
        snprintf(marker, sizeof marker, "%s/firefox-runtime/xkb", dir);
        setenv("XKB_CONFIG_ROOT", marker, 1);
    }

    setenv("HOME", home, 1);
    setenv("XDG_CONFIG_HOME", home, 0);
    setenv("XDG_CACHE_HOME", home, 0);
    setenv("XDG_RUNTIME_DIR", "/tmp/xdg", 0);
    setenv("WAYLAND_DISPLAY", "wayland-0", 0);
    setenv("XKB_CONFIG_ROOT", "/usr/share/X11/xkb", 0);
    setenv("GDK_BACKEND", "wayland", 1);
    setenv("MOZ_ENABLE_WAYLAND", "1", 1);
    setenv("MOZ_DBUS_REMOTE", "0", 1);
    setenv("MOZ_DISABLE_CONTENT_SANDBOX", "1", 1);
    setenv("MOZ_DISABLE_GMP_SANDBOX", "1", 1);
    setenv("MOZ_DISABLE_RDD_SANDBOX", "1", 1);
    setenv("MOZ_DISABLE_SOCKET_PROCESS_SANDBOX", "1", 1);
    setenv("MOZ_DISABLE_GPU_SANDBOX", "1", 1);
    setenv("LD_LIBRARY_PATH", libpath, 1);
    unsetenv("LD_PRELOAD");
    /* Wire-level trace: touch <app dir>/wayland-debug, launch once, then
     * delete the file. libwayland prints every message to stderr, which
     * is geckotv.log. */
    snprintf(marker, sizeof marker, "%s/wayland-debug", dir);
    if (access(marker, F_OK) == 0) {
        setenv("WAYLAND_DEBUG", "1", 1);
        fprintf(stderr, "geckotv WAYLAND_DEBUG=1 because %s exists\n", marker);
    }
    /* Extra environment for one run, e.g. MOZ_LOG=cubeb:4 to trace a media
     * problem: one KEY=VALUE per line in <app dir>/env. Delete the file
     * afterwards; the log it produces can grow fast. */
    snprintf(marker, sizeof marker, "%s/env", dir);
    {
        FILE *ef = fopen(marker, "r");
        if (ef) {
            char line[1024];
            while (fgets(line, sizeof line, ef)) {
                char *eq = strchr(line, '=');
                char *nl = strchr(line, '\n');
                if (nl)
                    *nl = '\0';
                if (!eq || line[0] == '#' || eq == line)
                    continue;
                *eq = '\0';
                setenv(line, eq + 1, 1);
                fprintf(stderr, "geckotv env %s=%s\n", line, eq + 1);
            }
            fclose(ef);
        }
    }
    setenv("APPID", "com.github.gprot42.geckotv", 0);

    /* Started by hand as root: Firefox refuses to run as root with a profile
     * owned by the app's own user and only says "Running Nightly as root". */
    {
        struct stat st;
        if (geteuid() == 0 && stat(home, &st) == 0 && st.st_uid != 0)
            fprintf(stderr, "geckotv: running as root, but the profile belongs to uid %u; "
                            "Firefox will refuse. Start the app from the TV (or ares-launch) "
                            "instead of running geckotv from a root shell.\n",
                    (unsigned)st.st_uid);
    }
    fprintf(stderr, "geckotv exec %s\n", firefox);
    args[narg++] = firefox;
    args[narg++] = "--profile";
    args[narg++] = home;
    args[narg++] = "--no-remote";
    /* Remote control for debugging from the Mac: touch <app dir>/marionette.
     * Off by default because it sets navigator.webdriver on every page, and
     * YouTube then stops sending video after about a minute. */
    snprintf(marker, sizeof marker, "%s/marionette", dir);
    if (access(marker, F_OK) == 0) {
        args[narg++] = "--marionette";
        /* Lets a Marionette client switch to chrome context, which is how
         * prefs are read and set on the live browser from the Mac. */
        args[narg++] = "-remote-allow-system-access";
        fprintf(stderr, "geckotv marionette on because %s exists\n", marker);
    }
    if (argc > 1 && argv[1][0] != '{') {
        int i;
        for (i = 1; i < argc && narg < 10; i++)
            args[narg++] = argv[i];
    } else if (argc > 1 && launch_target(argv[1], target, sizeof target)) {
        args[narg++] = target;
    } else {
        args[narg++] = "https://example.com";
    }
    args[narg] = NULL;
    /* Drop the Magic Remote wheel on the compositor so it scrolls this
     * window instead of leaving the app. Needs root; the boot script
     * covers the normal jail. */
    if (geteuid() == 0) {
        char mask[PATH_MAX];
        snprintf(mask, sizeof mask, "%s/mask-wheel", dir);
        if (access(mask, X_OK) == 0)
            system(mask);
    }
    if (access("/usr/bin/luna-send", X_OK) == 0) {
        system("/usr/bin/luna-send -n 1 -w 2000 "
               "luna://com.webos.service.mrcu/setHotkeyCursorPolicy "
               "'{\"button\":19,\"cursorPolicy\":2}' >/tmp/mrcu-policy.log 2>&1");
    }
    execv(firefox, args);
    perror("execv firefox");
    return 1;
}
