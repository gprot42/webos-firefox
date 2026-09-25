/* getrandom() for systems whose glibc predates it (2.25; webOS 4 has 2.24).
 *
 * Firefox's Rust code (the getrandom crate, used by uuid and others) looks
 * this function up at run time. Without it the crate falls back to reading
 * /dev/urandom, but only after opening /dev/random to wait for the kernel's
 * pool, and webOS 4's app jail has /dev/urandom but no /dev/random. The
 * crate then reports an error and Firefox aborts (uuid::Uuid::new_v4).
 *
 * This makes the system call (kernel 3.17 and later) and, if the kernel
 * lacks it, reads /dev/urandom. The launcher preloads it only when the TV's
 * glibc has no getrandom() of its own. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef SYS_getrandom
#if defined(__arm__)
#define SYS_getrandom 384
#elif defined(__i386__)
#define SYS_getrandom 355
#endif
#endif

__attribute__((visibility("default")))
ssize_t getrandom(void *buf, size_t len, unsigned int flags)
{
    ssize_t n = syscall(SYS_getrandom, buf, len, flags);
    int fd;

    if (n >= 0 || errno != ENOSYS)
        return n;
    fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    n = read(fd, buf, len);
    close(fd);
    return n;
}
