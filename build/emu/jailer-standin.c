/* Stand-in for /usr/bin/jailer in LG's webOS 6.0 emulator, which has none.
 * webOS's app manager (sam) starts every native app through jailer; without
 * it the launch fails silently. On a TV, jailer runs a Developer Mode app as
 * its own unprivileged user, and that is what this reproduces: the user and
 * group a webOS 6 TV gave this app (6795:5000), no chroot. Installed by
 * build/emu/emu.sh install; never part of a TV package.
 *
 * sam's arguments are logged to /tmp/jailer.log. The app is the first
 * argument after the options that is an absolute path; -p names its folder.
 *
 * 6.0's sam runs apps as root without jailer ("jail off"), so emu.sh install
 * also puts this program in place of the app's launcher, which it renames
 * geckotv.real: started as geckotv, it runs geckotv.real as the app's user. */
#define _GNU_SOURCE
#include <grp.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define APP_UID 6795
#define APP_GID 5000
/* The emulator's "compositor" group: the Wayland socket's folder is
 * root:compositor 0770. */
#define COMPOSITOR_GID 505

static int become_app_user(FILE *log)
{
    gid_t groups[] = { COMPOSITOR_GID };

    if (setgroups(1, groups) != 0 || setgid(APP_GID) != 0 || setuid(APP_UID) != 0) {
        if (log)
            fprintf(log, "jailer: cannot become %d:%d\n", APP_UID, APP_GID);
        return 0;
    }
    return 1;
}

int main(int argc, char **argv)
{
    FILE *log = fopen("/tmp/jailer.log", "a");
    const char *dir = NULL;
    const char *base = strrchr(argv[0], '/');
    int i, exe = 0;

    if (base && !strcmp(base, "/geckotv")) {
        char real[4096];

        snprintf(real, sizeof real, "%s.real", argv[0]);
        if (log)
            fprintf(log, "jailer: %s as %d:%d\n", real, APP_UID, APP_GID);
        if (!become_app_user(log))
            return 1;
        if (log)
            fclose(log);
        execv(real, argv);
        return 127;
    }

    for (i = 1; i < argc; i++) {
        if (log)
            fprintf(log, "%s%s", i > 1 ? " " : "jailer ", argv[i]);
        if (!strcmp(argv[i], "-p") && i + 1 < argc)
            dir = argv[i + 1];
        if (!exe && argv[i][0] == '/' && (i == 1 || argv[i - 1][0] != '-'))
            exe = i;
    }
    if (log)
        fprintf(log, "\n");
    if (!exe) {
        if (log)
            fprintf(log, "jailer: no program to run\n");
        return 1;
    }
    if (dir && chdir(dir) != 0 && log)
        fprintf(log, "jailer: cannot enter %s\n", dir);
    if (!become_app_user(log))
        return 1;
    if (log)
        fclose(log);
    execv(argv[exe], argv + exe);
    return 127;
}
