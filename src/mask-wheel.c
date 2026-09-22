#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef PTRACE_INTERRUPT
#define PTRACE_INTERRUPT 0x4207
#endif

/*
 * Hide Magic Remote wheel events from surface-manager's existing evdev fds.
 * EVIOCSMASK is per open file description, so reopening the device does nothing.
 * The call runs inside the compositor via its own Thumb __libc_do_syscall.
 *
 * A wrong cpsr (ARM mode on Thumb code) or a detach that leaves the hijacked
 * pc in place kills surface-manager. Every path restores registers first and
 * never continues a signal raised by the injected call.
 */

struct input_mask {
    uint32_t type;
    uint32_t codes_size;
    uint64_t codes_ptr;
};

#ifndef EVIOCSMASK
#define EVIOCSMASK _IOW('E', 0x93, struct input_mask)
#endif
#ifndef EVIOCGMASK
#define EVIOCGMASK _IOR('E', 0x92, struct input_mask)
#endif

_Static_assert(sizeof(struct input_mask) == 16, "input_mask size");
_Static_assert(EVIOCSMASK == 0x40104593u, "EVIOCSMASK");
_Static_assert(EVIOCGMASK == 0x80104592u, "EVIOCGMASK");
_Static_assert(EVIOCGNAME(64) == 0x80404506u, "EVIOCGNAME");

struct arm_regs {
    unsigned long uregs[18];
};

#define ARM_r0 0
#define ARM_r12 12
#define ARM_sp 13
#define ARM_lr 14
#define ARM_pc 15
#define ARM_cpsr 16

#define DO_SYSCALL_OFF 0x21ca0u
#define NR_IOCTL 54

static const unsigned char k_syscall_bytes[8] = {
    0x80, 0xb5, 0x67, 0x46, 0x00, 0xdf, 0x80, 0xbd
};

static pid_t g_tid;
static volatile sig_atomic_t g_attached;
static volatile sig_atomic_t g_dirty;
static volatile sig_atomic_t g_kill_tracee;
static struct arm_regs g_saved;

static void bail(int sig)
{
    int status;

    if (g_attached) {
        if (g_dirty) {
            ptrace(PTRACE_INTERRUPT, g_tid, NULL, NULL);
            waitpid(g_tid, &status, WNOHANG);
            ptrace(PTRACE_SETREGS, g_tid, NULL, &g_saved);
            g_dirty = 0;
        }
        ptrace(PTRACE_DETACH, g_tid, NULL, NULL);
        g_attached = 0;
    }
    if (g_kill_tracee && g_tid > 0)
        kill(g_tid, SIGKILL);
    _exit(128 + sig);
}

static void arm_bail(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = bail;
    sigaction(SIGALRM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

static void mark_dirty(const struct arm_regs *saved)
{
    g_saved = *saved;
    g_dirty = 1;
    __asm__ volatile("" ::: "memory");
}

static int poke(pid_t pid, unsigned long addr, const void *buf, size_t len)
{
    const unsigned char *p = buf;
    size_t i;

    for (i = 0; i < len; i += sizeof(long)) {
        unsigned long word = 0;
        size_t n = len - i;

        if (n > sizeof word)
            n = sizeof word;
        memcpy(&word, p + i, n);
        if (ptrace(PTRACE_POKEDATA, pid, (void *)(addr + i), (void *)word) < 0)
            return -1;
    }
    return 0;
}

static int peek(pid_t pid, unsigned long addr, void *buf, size_t len)
{
    unsigned char *p = buf;
    size_t i;

    for (i = 0; i < len; i += sizeof(long)) {
        unsigned long word;
        size_t n = len - i;

        errno = 0;
        word = ptrace(PTRACE_PEEKDATA, pid, (void *)(addr + i), NULL);
        if (errno)
            return -1;
        if (n > sizeof word)
            n = sizeof word;
        memcpy(p + i, &word, n);
    }
    return 0;
}

static int parse_maps(pid_t pid, int (*cb)(unsigned long, unsigned long, const char *, unsigned long, const char *, void *), void *ctx)
{
    FILE *f;
    char path[64];
    char line[512];

    snprintf(path, sizeof path, "/proc/%d/maps", pid);
    f = fopen(path, "r");
    if (!f)
        return -1;
    while (fgets(line, sizeof line, f)) {
        unsigned long start, end, off;
        char perms[8];
        char file[256];
        int n;

        file[0] = 0;
        n = sscanf(line, "%lx-%lx %7s %lx %*s %*s %255s", &start, &end, perms, &off, file);
        if (n < 4)
            continue;
        if (cb(start, end, perms, off, file, ctx)) {
            fclose(f);
            return 1;
        }
    }
    fclose(f);
    return 0;
}

struct range_query {
    unsigned long addr;
    size_t len;
    int need_write;
    int hit;
};

static int range_cb(unsigned long start, unsigned long end, const char *perms, unsigned long off, const char *file, void *ctx)
{
    struct range_query *q = ctx;
    (void)off;
    (void)file;
    if (start <= q->addr && q->addr + q->len <= end) {
        if (q->need_write && perms[1] != 'w')
            return 0;
        q->hit = 1;
        return 1;
    }
    return 0;
}

static int range_ok(pid_t pid, unsigned long addr, size_t len, int need_write)
{
    struct range_query q;

    q.addr = addr;
    q.len = len;
    q.need_write = need_write;
    q.hit = 0;
    parse_maps(pid, range_cb, &q);
    return q.hit;
}

struct libc_query {
    unsigned long addr;
};

static int libc_cb(unsigned long start, unsigned long end, const char *perms, unsigned long off, const char *file, void *ctx)
{
    struct libc_query *q = ctx;
    unsigned long addr;

    if (perms[2] != 'x' || !strstr(file, "/lib/libc.so"))
        return 0;
    if (off > DO_SYSCALL_OFF)
        return 0;
    addr = start + (DO_SYSCALL_OFF - off);
    if (addr < start || addr + sizeof k_syscall_bytes > end)
        return 0;
    q->addr = addr;
    return 1;
}

static int syscall_addr(pid_t pid, unsigned long *out)
{
    struct libc_query q;
    unsigned char got[8];

    q.addr = 0;
    if (!parse_maps(pid, libc_cb, &q) || !q.addr)
        return -1;
    if (peek(pid, q.addr, got, sizeof got) < 0)
        return -1;
    if (memcmp(got, k_syscall_bytes, sizeof got) != 0) {
        fprintf(stderr, "libc syscall bytes mismatch at %lx\n", q.addr);
        return -1;
    }
    *out = q.addr;
    return 0;
}

static char task_state(pid_t pid)
{
    char path[64];
    char buf[512];
    char *paren;
    FILE *f;

    snprintf(path, sizeof path, "/proc/%d/stat", pid);
    f = fopen(path, "r");
    if (!f)
        return '?';
    if (!fgets(buf, sizeof buf, f)) {
        fclose(f);
        return '?';
    }
    fclose(f);
    paren = strrchr(buf, ')');
    if (!paren || paren[1] != ' ' || !paren[2])
        return '?';
    return paren[2];
}

static int regs_match(pid_t tid, const struct arm_regs *saved)
{
    struct arm_regs now;

    if (ptrace(PTRACE_GETREGS, tid, NULL, &now) < 0)
        return 0;
    return now.uregs[ARM_pc] == saved->uregs[ARM_pc] &&
           now.uregs[ARM_sp] == saved->uregs[ARM_sp];
}

/* 0: stopped and registers restored. -1: restored, call failed. -2: not safe. */
static int remote_ioctl(pid_t tid, unsigned long fn, unsigned long hole,
                        int fd, unsigned long request, unsigned long arg,
                        long *ret, int *sig_out, unsigned long *pc_out)
{
    struct arm_regs regs;
    struct arm_regs saved;
    struct arm_regs after;
    int status = 0;

    memset(&after, 0, sizeof after);
    if (ptrace(PTRACE_GETREGS, tid, NULL, &saved) < 0)
        return -2;
    mark_dirty(&saved);
    regs = saved;
    regs.uregs[ARM_r0] = (unsigned long)fd;
    regs.uregs[1] = request;
    regs.uregs[2] = arg;
    regs.uregs[ARM_r12] = NR_IOCTL;
    regs.uregs[ARM_lr] = hole;
    regs.uregs[ARM_pc] = fn & ~1UL;
    regs.uregs[ARM_cpsr] |= 1UL << 5;
    if (ptrace(PTRACE_SETREGS, tid, NULL, &regs) < 0) {
        perror("setregs");
        goto restore;
    }
    alarm(3);
    if (ptrace(PTRACE_CONT, tid, NULL, NULL) < 0) {
        perror("cont");
        goto restore;
    }
    if (waitpid(tid, &status, 0) < 0) {
        perror("wait");
        goto restore;
    }
    alarm(0);
    if (ptrace(PTRACE_GETREGS, tid, NULL, &after) < 0)
        goto restore;
    if (ret)
        *ret = (long)after.uregs[ARM_r0];
    if (sig_out)
        *sig_out = WIFSTOPPED(status) ? WSTOPSIG(status) : -1;
    if (pc_out)
        *pc_out = after.uregs[ARM_pc];
    if (ptrace(PTRACE_SETREGS, tid, NULL, &saved) < 0)
        goto restore;
    g_dirty = 0;
    if (!regs_match(tid, &saved))
        return -2;
    if (!WIFSTOPPED(status))
        return -1;
    if ((after.uregs[ARM_pc] & ~1UL) >= 0x2000)
        return -1;
    return 0;

restore:
    alarm(0);
    if (ptrace(PTRACE_SETREGS, tid, NULL, &saved) < 0) {
        ptrace(PTRACE_INTERRUPT, tid, NULL, NULL);
        waitpid(tid, &status, WNOHANG);
        ptrace(PTRACE_SETREGS, tid, NULL, &saved);
    }
    g_dirty = 0;
    if (!regs_match(tid, &saved))
        return -2;
    return -1;
}

static unsigned long stack_slot(pid_t tid)
{
    struct arm_regs regs;
    unsigned long slot;

    if (ptrace(PTRACE_GETREGS, tid, NULL, &regs) < 0)
        return 0;
    slot = (regs.uregs[ARM_sp] - 640) & ~7UL;
    if (!range_ok(tid, slot, 400, 1))
        return 0;
    /* __libc_do_syscall pushes 8 bytes at sp. Keep the buffer clear of that. */
    if (slot + 400 > regs.uregs[ARM_sp] - 8)
        return 0;
    return slot;
}

static unsigned long fault_hole(pid_t pid)
{
    unsigned long addr;

    for (addr = 0x1000; addr < 0x2000; addr += 0x1000) {
        if (!range_ok(pid, addr, 4, 0))
            return addr;
    }
    return 0;
}

static int device_has_wheel(const char *path)
{
    int fd;
    unsigned long rel[2];

    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return 0;
    memset(rel, 0, sizeof rel);
    if (ioctl(fd, EVIOCGBIT(EV_REL, sizeof rel), rel) < 0) {
        close(fd);
        return 0;
    }
    close(fd);
    return (rel[0] & (1UL << REL_WHEEL)) != 0;
}

static int attach_tid(pid_t tid)
{
    int status;
    char state;

    state = task_state(tid);
    if (state != 'S' && state != 'R') {
        fprintf(stderr, "tid %d state %c, not attaching\n", tid, state);
        return -1;
    }
    if (ptrace(PTRACE_ATTACH, tid, NULL, NULL) < 0) {
        perror("attach");
        return -1;
    }
    g_tid = tid;
    g_attached = 1;
    alarm(3);
    if (waitpid(tid, &status, 0) < 0) {
        perror("attach-wait");
        return -1;
    }
    alarm(0);
    if (!WIFSTOPPED(status)) {
        fprintf(stderr, "attach did not stop\n");
        return -1;
    }
    return 0;
}

static void detach_tid(void)
{
    if (!g_attached)
        return;
    if (g_dirty) {
        ptrace(PTRACE_SETREGS, g_tid, NULL, &g_saved);
        g_dirty = 0;
    }
    ptrace(PTRACE_DETACH, g_tid, NULL, NULL);
    g_attached = 0;
}

static int read_name(pid_t tid, int fd, unsigned long slot, unsigned long fn,
                     unsigned long hole, char *name, size_t name_len)
{
    struct input_mask mask;
    unsigned char buf[64];
    long rc = -1;
    int sig = 0;
    unsigned long pc = 0;
    int err;

    memset(buf, 0, sizeof buf);
    memset(&mask, 0, sizeof mask);
    if (poke(tid, slot, buf, sizeof buf) < 0)
        return -1;
    err = remote_ioctl(tid, fn, hole, fd, EVIOCGNAME(sizeof buf), slot, &rc, &sig, &pc);
    if (peek(tid, slot, buf, sizeof buf) < 0)
        return -2;
    buf[sizeof buf - 1] = 0;
    snprintf(name, name_len, "%s", (char *)buf);
    if (err == -2)
        return -2;
    if (err < 0 || rc < 0) {
        fprintf(stderr, "name fd %d rc %ld sig %d pc %lx\n", fd, rc, sig, pc);
        return -1;
    }
    return 0;
}

static int set_type_mask(pid_t tid, int fd, unsigned long slot, unsigned long fn,
                         unsigned long hole, uint32_t type, const unsigned char *bits,
                         size_t bits_len)
{
    struct input_mask mask;
    long rc = -1;
    int sig = 0;
    unsigned long pc = 0;
    int err;

    memset(&mask, 0, sizeof mask);
    mask.type = type;
    mask.codes_size = bits_len;
    mask.codes_ptr = slot;
    if (poke(tid, slot, bits, bits_len) < 0 ||
        poke(tid, slot + 128, &mask, sizeof mask) < 0)
        return -1;
    err = remote_ioctl(tid, fn, hole, fd, EVIOCSMASK, slot + 128, &rc, &sig, &pc);
    if (err == -2)
        return -2;
    if (err < 0 || rc != 0) {
        fprintf(stderr, "mask type %u fd %d rc %ld sig %d pc %lx\n", type, fd, rc, sig, pc);
        return -1;
    }
    return 0;
}

static int set_wheel_mask(pid_t tid, int fd, unsigned long slot, unsigned long fn, unsigned long hole)
{
    unsigned char bits[16];

    memset(bits, 0, sizeof bits);
    return set_type_mask(tid, fd, slot, fn, hole, EV_REL, bits, sizeof bits);
}

static int pass_wheel(pid_t tid, int fd, unsigned long slot, unsigned long fn, unsigned long hole)
{
    unsigned char bits[16];

    memset(bits, 0xff, sizeof bits);
    return set_type_mask(tid, fd, slot, fn, hole, EV_REL, bits, sizeof bits);
}

/* KEY_CNT is 768. Clear Enter and OK so the wheel click is not a leave-app key. */
static int set_ok_mask(pid_t tid, int fd, unsigned long slot, unsigned long fn,
                       unsigned long hole, int block)
{
    unsigned char bits[96];
    unsigned int codes[] = { 28, 352 };
    unsigned int i;

    memset(bits, 0xff, sizeof bits);
    if (block) {
        for (i = 0; i < sizeof codes / sizeof codes[0]; i++) {
            unsigned int code = codes[i];
            bits[code / 8] &= (unsigned char)~(1u << (code % 8));
        }
    }
    return set_type_mask(tid, fd, slot, fn, hole, EV_KEY, bits, sizeof bits);
}

/* Key 1198 is CursorShow, sent with a real wheel notch. Ask the kernel
 * whether a bitmap that large is stored. */
static int restore_input(pid_t tid, int fd, unsigned long slot, unsigned long fn, unsigned long hole)
{
    unsigned char keys[256];
    unsigned char rel[16];
    struct input_mask mask;
    long rc = -1;
    int sig = 0;
    unsigned long pc = 0;
    int err;

    memset(rel, 0xff, sizeof rel);
    memset(&mask, 0, sizeof mask);
    mask.type = EV_REL;
    mask.codes_size = sizeof rel;
    mask.codes_ptr = slot;
    if (poke(tid, slot, rel, sizeof rel) < 0 ||
        poke(tid, slot + 320, &mask, sizeof mask) < 0)
        return -1;
    err = remote_ioctl(tid, fn, hole, fd, EVIOCSMASK, slot + 320, &rc, &sig, &pc);
    if (err < 0)
        return err;
    memset(keys, 0xff, sizeof keys);
    mask.type = EV_KEY;
    mask.codes_size = sizeof keys;
    mask.codes_ptr = slot;
    if (poke(tid, slot, keys, sizeof keys) < 0 ||
        poke(tid, slot + 320, &mask, sizeof mask) < 0)
        return -1;
    err = remote_ioctl(tid, fn, hole, fd, EVIOCSMASK, slot + 320, &rc, &sig, &pc);
    printf("restored %d rc %ld\n", fd, rc);
    return err < 0 ? err : 0;
}

static int probe_cursor_show(pid_t tid, int fd, unsigned long slot, unsigned long fn, unsigned long hole)
{
    unsigned char bits[256];
    struct input_mask mask;
    long rc = -1;
    int sig = 0;
    unsigned long pc = 0;
    int err;
    unsigned int code = 1198;

    memset(bits, 0xff, sizeof bits);
    bits[code / 8] &= (unsigned char)~(1u << (code % 8));
    memset(&mask, 0, sizeof mask);
    mask.type = EV_KEY;
    mask.codes_size = sizeof bits;
    mask.codes_ptr = slot;
    if (poke(tid, slot, bits, sizeof bits) < 0 ||
        poke(tid, slot + 320, &mask, sizeof mask) < 0)
        return -1;
    err = remote_ioctl(tid, fn, hole, fd, EVIOCSMASK, slot + 320, &rc, &sig, &pc);
    printf("set cursor-show mask fd %d rc %ld sig %d\n", fd, rc, sig);
    if (err == -2)
        return -2;
    memset(bits, 0, sizeof bits);
    mask.codes_ptr = slot;
    if (poke(tid, slot, bits, sizeof bits) < 0 ||
        poke(tid, slot + 320, &mask, sizeof mask) < 0)
        return -1;
    err = remote_ioctl(tid, fn, hole, fd, EVIOCGMASK, slot + 320, &rc, &sig, &pc);
    if (peek(tid, slot, bits, 160) < 0)
        return -2;
    printf("get cursor-show fd %d rc %ld byte149 %02x bit %d\n",
           fd, rc, bits[code / 8],
           (bits[code / 8] >> (code % 8)) & 1);
    return err;
}

static int wheel_bit_set(pid_t tid, int fd, unsigned long slot, unsigned long fn, unsigned long hole, int *set)
{
    unsigned char bits[16];
    struct input_mask mask;
    long rc = -1;
    int sig = 0;
    unsigned long pc = 0;
    int err;

    memset(bits, 0, sizeof bits);
    memset(&mask, 0, sizeof mask);
    mask.type = EV_REL;
    mask.codes_size = sizeof bits;
    mask.codes_ptr = slot;
    if (poke(tid, slot + 32, &mask, sizeof mask) < 0 ||
        poke(tid, slot, bits, sizeof bits) < 0)
        return -1;
    err = remote_ioctl(tid, fn, hole, fd, EVIOCGMASK, slot + 32, &rc, &sig, &pc);
    if (peek(tid, slot, bits, sizeof bits) < 0)
        return -2;
    if (err == -2)
        return -2;
    if (err < 0 || rc != 0) {
        fprintf(stderr, "check fd %d rc %ld sig %d pc %lx bytes %02x %02x\n",
                fd, rc, sig, pc, bits[0], bits[1]);
        return -1;
    }
    *set = (bits[REL_WHEEL / 8] & (1u << (REL_WHEEL % 8))) != 0;
    return 0;
}

static int open_wheel_fd(void)
{
    const char *cands[] = {
        "/dev/input/event2", "/dev/input/event3", "/dev/input/event4", NULL
    };
    int i;

    for (i = 0; cands[i]; i++) {
        if (device_has_wheel(cands[i])) {
            int fd = open(cands[i], O_RDONLY | O_CLOEXEC);
            if (fd >= 0)
                return fd;
        }
    }
    return -1;
}

static pid_t surface_manager_pid(void)
{
    FILE *f;
    pid_t pid = 0;

    f = popen("pidof surface-manager", "r");
    if (!f)
        return 0;
    if (fscanf(f, "%d", &pid) != 1)
        pid = 0;
    pclose(f);
    return pid;
}

static int self_test(void)
{
    int pipes[2];
    pid_t child;
    pid_t sm;
    pid_t sm_after;
    char fdtext[32];
    char name[64];
    unsigned long fn = 0;
    unsigned long hole;
    unsigned long slot;
    int n;
    int fd = -1;
    int wheel = 1;
    int err;

    sm = surface_manager_pid();
    if (pipe(pipes) < 0)
        return 1;
    child = fork();
    if (child < 0)
        return 1;
    if (child == 0) {
        fd = open_wheel_fd();
        n = snprintf(fdtext, sizeof fdtext, "%d\n", fd);
        if (write(pipes[1], fdtext, (size_t)n) < 0)
            _exit(1);
        close(pipes[0]);
        close(pipes[1]);
        if (fd < 0)
            _exit(1);
        if (ptrace(PTRACE_TRACEME, 0, NULL, NULL) < 0)
            _exit(1);
        raise(SIGSTOP);
        for (;;)
            pause();
    }
    close(pipes[1]);
    g_kill_tracee = 1;
    n = read(pipes[0], fdtext, sizeof fdtext - 1);
    close(pipes[0]);
    if (n <= 0) {
        kill(child, SIGKILL);
        return 1;
    }
    fdtext[n] = 0;
    fd = atoi(fdtext);
    if (fd < 0) {
        fprintf(stderr, "self-test: no wheel device\n");
        kill(child, SIGKILL);
        waitpid(child, NULL, 0);
        return 1;
    }
    alarm(3);
    if (waitpid(child, &n, 0) < 0) {
        kill(child, SIGKILL);
        return 1;
    }
    alarm(0);
    g_tid = child;
    g_attached = 1;
    err = 1;
    if (syscall_addr(child, &fn) < 0)
        goto done;
    hole = fault_hole(child);
    slot = stack_slot(child);
    if (!hole || !slot) {
        fprintf(stderr, "self-test: hole %lx slot %lx\n", hole, slot);
        goto done;
    }
    if (read_name(child, fd, slot, fn, hole, name, sizeof name) < 0)
        goto done;
    printf("self-test name: %s\n", name);
    if (!strstr(name, "LGE") && !strstr(name, "RCU")) {
        fprintf(stderr, "self-test: unexpected device name\n");
        goto done;
    }
    if (set_wheel_mask(child, fd, slot, fn, hole) < 0)
        goto done;
    if (wheel_bit_set(child, fd, slot, fn, hole, &wheel) < 0)
        goto done;
    printf("self-test wheel-bit %d\n", wheel);
    if (wheel)
        goto done;
    err = 0;
done:
    detach_tid();
    if (task_state(child) != 'S' && task_state(child) != 'R') {
        fprintf(stderr, "self-test: child state %c\n", task_state(child));
        err = 1;
    } else {
        printf("self-test child alive\n");
    }
    kill(child, SIGKILL);
    waitpid(child, NULL, 0);
    g_kill_tracee = 0;
    sm_after = surface_manager_pid();
    if (sm && sm_after != sm) {
        fprintf(stderr, "self-test changed surface-manager %d -> %d\n", sm, sm_after);
        err = 1;
    }
    return err;
}

static int for_each_wheel_fd(pid_t pid, int mode)
{
    unsigned long fn = 0;
    unsigned long hole;
    unsigned long slot;
    DIR *dir;
    struct dirent *ent;
    char path[128];
    int touched = 0;
    int failed = 0;

    if (syscall_addr(pid, &fn) < 0) {
        fprintf(stderr, "syscall not found\n");
        return -1;
    }
    hole = fault_hole(pid);
    slot = stack_slot(pid);
    if (!hole || !slot) {
        fprintf(stderr, "hole %lx slot %lx\n", hole, slot);
        return -1;
    }
    snprintf(path, sizeof path, "/proc/%d/fd", pid);
    dir = opendir(path);
    if (!dir)
        return -1;
    while ((ent = readdir(dir)) != NULL) {
        char fdpath[160];
        char link[64];
        char name[64];
        int n;
        int evfd;
        int wheel = 1;
        int err;

        if (!isdigit((unsigned char)ent->d_name[0]))
            continue;
        snprintf(fdpath, sizeof fdpath, "/proc/%d/fd/%s", pid, ent->d_name);
        n = readlink(fdpath, link, sizeof link - 1);
        if (n < 0)
            continue;
        link[n] = 0;
        if (strncmp(link, "/dev/input/event", 16) != 0)
            continue;
        if (!device_has_wheel(link))
            continue;
        evfd = atoi(ent->d_name);
        err = read_name(pid, evfd, slot, fn, hole, name, sizeof name);
        if (err == -2) {
            failed = -2;
            break;
        }
        if (err < 0) {
            failed = 1;
            continue;
        }
        printf("name %s fd %d: %s\n", link, evfd, name);
        if (mode == 8) {
            err = restore_input(pid, evfd, slot, fn, hole);
            if (err == -2) {
                failed = -2;
                break;
            }
            if (err == 0)
                touched++;
            continue;
        }
        if (mode == 7) {
            err = probe_cursor_show(pid, evfd, slot, fn, hole);
            if (err == -2) {
                failed = -2;
                break;
            }
            touched++;
            continue;
        }
        if (mode == 0)
            continue;
        if (mode == 5) {
            err = pass_wheel(pid, evfd, slot, fn, hole);
            if (err == -2) {
                failed = -2;
                break;
            }
            if (err < 0) {
                failed = 1;
                continue;
            }
            err = set_ok_mask(pid, evfd, slot, fn, hole, 0);
            if (err == -2) {
                failed = -2;
                break;
            }
            if (err < 0) {
                failed = 1;
                continue;
            }
            err = wheel_bit_set(pid, evfd, slot, fn, hole, &wheel);
            if (err == -2) {
                failed = -2;
                break;
            }
            printf("restored %s wheel-bit %d\n", link, wheel);
            if (err == 0 && wheel)
                touched++;
            continue;
        }
        if (mode == 6) {
            err = pass_wheel(pid, evfd, slot, fn, hole);
            if (err == -2) {
                failed = -2;
                break;
            }
            if (err < 0) {
                failed = 1;
                continue;
            }
            err = wheel_bit_set(pid, evfd, slot, fn, hole, &wheel);
            if (err == -2) {
                failed = -2;
                break;
            }
            printf("wheel passed %s bit %d\n", link, wheel);
            if (err == 0 && wheel)
                touched++;
            continue;
        }
        if (mode == 2) {
            err = wheel_bit_set(pid, evfd, slot, fn, hole, &wheel);
            if (err == -2) {
                failed = -2;
                break;
            }
            if (err < 0) {
                failed = 1;
                continue;
            }
            printf("check %s wheel-bit %d\n", link, wheel);
            if (!wheel)
                touched++;
            continue;
        }
        err = set_wheel_mask(pid, evfd, slot, fn, hole);
        if (err == -2) {
            failed = -2;
            break;
        }
        if (err < 0) {
            failed = 1;
            continue;
        }
        if (mode == 1) {
            err = probe_cursor_show(pid, evfd, slot, fn, hole);
            if (err == -2) {
                failed = -2;
                break;
            }
        }
        if (mode == 3 || mode == 4) {
            err = set_ok_mask(pid, evfd, slot, fn, hole, mode == 3);
            if (err == -2) {
                failed = -2;
                break;
            }
            if (err < 0) {
                failed = 1;
                continue;
            }
            printf("%s ok-key on %s\n", mode == 3 ? "blocked" : "restored", link);
        }
        err = wheel_bit_set(pid, evfd, slot, fn, hole, &wheel);
        if (err == -2) {
            failed = -2;
            break;
        }
        if (err < 0 || wheel) {
            fprintf(stderr, "mask did not stick on %s\n", link);
            failed = 1;
            continue;
        }
        printf("masked wheel on %s\n", link);
        touched++;
    }
    closedir(dir);
    if (failed == -2)
        return -2;
    if (mode == 0)
        return failed ? -1 : 0;
    printf("masked %d\n", touched);
    if (failed || touched == 0)
        return -1;
    return 0;
}

static int run_on_compositor(int mode);

static int gecko_is_front(void)
{
    FILE *f;
    char buf[12288];
    size_t n;
    const char *p;
    const char *last = NULL;
    const char *line;

    f = fopen("/var/log/messages", "r");
    if (!f)
        return 0;
    if (fseek(f, -8192, SEEK_END) < 0)
        rewind(f);
    n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    if (n == 0)
        return 0;
    buf[n] = 0;
    for (p = buf; (p = strstr(p, "\"visible\":true")) != NULL; p += 14)
        last = p;
    if (!last)
        return 0;
    line = last;
    while (line > buf && line[-1] != '\n')
        line--;
    return strstr(line, "com.github.gprot42.geckotv") != NULL;
}

static int watch_compositor(void)
{
    int masked = 0;
    int absent = 0;

    for (;;) {
        int front = gecko_is_front();

        if (front) {
            absent = 0;
            if (!masked) {
                printf("gecko front, hide wheel from compositor\n");
                if (run_on_compositor(1) == 0)
                    masked = 1;
            }
        } else if (masked) {
            /* The exit animation flashes an empty card. Ignore that. */
            absent++;
            if (absent >= 8) {
                printf("gecko not front, restore wheel\n");
                if (run_on_compositor(6) == 0)
                    masked = 0;
                absent = 0;
            }
        }
        usleep(250000);
    }
}

static int run_on_compositor(int mode)
{
    pid_t pid;
    pid_t after;
    int rc;

    pid = surface_manager_pid();
    if (pid <= 0) {
        fprintf(stderr, "no surface-manager\n");
        return 1;
    }
    printf("surface-manager %d\n", pid);
    if (attach_tid(pid) < 0) {
        detach_tid();
        return 1;
    }
    rc = for_each_wheel_fd(pid, mode);
    detach_tid();
    after = surface_manager_pid();
    if (after != pid) {
        fprintf(stderr, "surface-manager changed %d -> %d\n", pid, after);
        return 1;
    }
    printf("surface-manager still %d state %c\n", after, task_state(after));
    return rc == 0 ? 0 : 1;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "--apply";

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    arm_bail();
    if (strcmp(mode, "--self-test") == 0)
        return self_test();
    if (strcmp(mode, "--names") == 0)
        return run_on_compositor(0);
    if (strcmp(mode, "--check") == 0)
        return run_on_compositor(2);
    if (strcmp(mode, "--apply") == 0)
        return run_on_compositor(1);
    if (strcmp(mode, "--block-ok") == 0)
        return run_on_compositor(3);
    if (strcmp(mode, "--allow-ok") == 0)
        return run_on_compositor(4);
    if (strcmp(mode, "--restore") == 0)
        return run_on_compositor(5);
    if (strcmp(mode, "--pass-wheel") == 0)
        return run_on_compositor(6);
    if (strcmp(mode, "--watch") == 0)
        return watch_compositor();
    if (strcmp(mode, "--probe-1198") == 0)
        return run_on_compositor(7);
    if (strcmp(mode, "--unbreak") == 0)
        return run_on_compositor(8);
    fprintf(stderr, "usage: mask-wheel [--self-test|--names|--check|--apply|--restore|--pass-wheel|--watch]\n");
    return 2;
}
