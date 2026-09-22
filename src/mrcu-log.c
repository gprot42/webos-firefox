#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Shared read of the Magic Remote. No grab, no mask. One line per event,
 * plus any new system-log lines, so a wheel gesture can be matched to the
 * moment the browser card disappears.
 */

#define NDEV 10

static const char *paths[NDEV] = {
    "/dev/input/event0", "/dev/input/event1", "/dev/input/event2",
    "/dev/input/event3", "/dev/input/event4", "/dev/input/event5",
    "/dev/input/event6", "/dev/input/event7", "/dev/input/event8",
    "/dev/input/event9"
};

static int last_abs[NDEV][2];
static struct timespec last_abs_ts[NDEV];

static const char *type_name(unsigned type)
{
    switch (type) {
    case EV_SYN: return "SYN";
    case EV_KEY: return "KEY";
    case EV_REL: return "REL";
    case EV_ABS: return "ABS";
    case EV_MSC: return "MSC";
    default: return "EV";
    }
}

static const char *code_name(unsigned type, unsigned code)
{
    if (type == EV_REL && code == REL_WHEEL)
        return "WHEEL";
    if (type == EV_REL && code == REL_HWHEEL)
        return "HWHEEL";
    if (type == EV_KEY) {
        switch (code) {
        case 28: return "ENTER";
        case 158: return "BACK";
        case 172: return "HOME";
        case 174: return "EXIT";
        case 272: return "BTN_LEFT";
        case 352: return "OK";
        case 103: return "UP";
        case 108: return "DOWN";
        case 105: return "LEFT";
        case 106: return "RIGHT";
        case 1198: return "CURSOR_SHOW";
        case 1199: return "CURSOR_HIDE";
        case 1232: return "WHEEL_CUSTOM";
        default: break;
        }
    }
    if (type == EV_ABS && code == ABS_X)
        return "X";
    if (type == EV_ABS && code == ABS_Y)
        return "Y";
    return "";
}

static void stamp(char *buf, size_t len)
{
    struct timespec ts;
    struct tm tm;

    clock_gettime(CLOCK_REALTIME, &ts);
    gmtime_r(&ts.tv_sec, &tm);
    snprintf(buf, len, "%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000);
}

static void flush_messages(FILE *msg, off_t *pos)
{
    char line[512];
    struct stat st;

    if (!msg)
        return;
    if (fstat(fileno(msg), &st) == 0 && st.st_size < *pos) {
        clearerr(msg);
        fseeko(msg, 0, SEEK_SET);
        *pos = 0;
    }
    while (fgets(line, sizeof line, msg)) {
        char when[40];
        size_t n = strlen(line);

        if (n && line[n - 1] == '\n')
            line[n - 1] = 0;
        stamp(when, sizeof when);
        printf("%s SYS %s\n", when, line);
        *pos = ftello(msg);
    }
    clearerr(msg);
}

int main(void)
{
    struct pollfd pfds[NDEV];
    char name[NDEV][64];
    FILE *msg;
    off_t msg_pos = 0;
    int i;

    setvbuf(stdout, NULL, _IOLBF, 0);
    memset(last_abs, 0xff, sizeof last_abs);
    for (i = 0; i < NDEV; i++) {
        char buf[64];

        pfds[i].fd = open(paths[i], O_RDONLY | O_CLOEXEC | O_NONBLOCK);
        pfds[i].events = POLLIN;
        name[i][0] = 0;
        if (pfds[i].fd < 0) {
            fprintf(stderr, "open %s errno %d\n", paths[i], errno);
            continue;
        }
        if (ioctl(pfds[i].fd, EVIOCGNAME(sizeof buf - 1), buf) >= 0) {
            buf[sizeof buf - 1] = 0;
            snprintf(name[i], sizeof name[i], "%s", buf);
        }
        printf("open %s fd %d %s\n", paths[i], pfds[i].fd, name[i]);
    }
    msg = fopen("/var/log/messages", "r");
    if (msg) {
        fseeko(msg, 0, SEEK_END);
        msg_pos = ftello(msg);
    } else {
        fprintf(stderr, "messages errno %d\n", errno);
    }
    for (;;) {
        if (poll(pfds, NDEV, 200) < 0) {
            if (errno == EINTR)
                continue;
            perror("poll");
            return 1;
        }
        for (i = 0; i < NDEV; i++) {
            struct input_event ev;

            if (pfds[i].fd < 0 || !(pfds[i].revents & POLLIN))
                continue;
            while (read(pfds[i].fd, &ev, sizeof ev) == (ssize_t)sizeof ev) {
                char when[40];
                const char *pretty;

                if (ev.type == EV_SYN)
                    continue;
                if (ev.type == EV_ABS && (ev.code == ABS_X || ev.code == ABS_Y)) {
                    struct timespec now;
                    int slot = ev.code == ABS_X ? 0 : 1;
                    long ms;

                    clock_gettime(CLOCK_MONOTONIC, &now);
                    ms = (now.tv_sec - last_abs_ts[i].tv_sec) * 1000
                        + (now.tv_nsec - last_abs_ts[i].tv_nsec) / 1000000;
                    if (last_abs[i][slot] == ev.value && ms < 200)
                        continue;
                    last_abs[i][slot] = ev.value;
                    last_abs_ts[i] = now;
                }
                pretty = code_name(ev.type, ev.code);
                stamp(when, sizeof when);
                printf("%s DEV %s %s type %u %s code %u %s value %d\n",
                       when, paths[i], name[i], ev.type, type_name(ev.type),
                       ev.code, pretty, ev.value);
            }
        }
        flush_messages(msg, &msg_pos);
    }
}
