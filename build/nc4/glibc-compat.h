/* Constants that the TV kernels support but glibc 2.12's headers (the nc4
 * sysroot) predate. Included into every C and C++ file of the nc4 build via
 * CFLAGS/CXXFLAGS in build/mozconfig-nc4. Values are the kernel's own
 * (asm-generic, which ARM uses).
 *
 * Include no system header here: this file comes before each source file's
 * own #define _GNU_SOURCE, and including e.g. <sys/mman.h> first would lock
 * in the default feature set and hide accept4(), pipe2(), mremap() and more.
 * A plain #define is safe even if a later header defines the same value. */
#ifndef WEBOS_GLIBC_COMPAT_H
#define WEBOS_GLIBC_COMPAT_H

/* <sys/mman.h> */
#define MADV_HUGEPAGE 14 /* Linux 2.6.38 */
#define MADV_NOHUGEPAGE 15
#define MADV_DONTDUMP 16 /* Linux 3.4 */
#define MADV_DODUMP 17

/* <fcntl.h> */
#define O_PATH 010000000 /* Linux 2.6.39 */
#define AT_NO_AUTOMOUNT 0x800 /* Linux 2.6.38 */
#define AT_EMPTY_PATH 0x1000 /* Linux 2.6.39 */

#ifndef __cplusplus
/* secure_getenv() is glibc 2.17. glibc 2.12 exports the same function as
 * __secure_getenv, and newer glibc keeps exporting __secure_getenv@GLIBC_2.4
 * as a compatibility symbol (checked on the TV's glibc 2.35), so this works
 * on old and new TVs. Used by NSS (C). */
/* Firefox compiles with hidden visibility by default; these are glibc's, so
 * they must stay default or the linker looks for them inside our library. */
#define WEBOS_GLIBC_FN extern __attribute__((visibility("default")))
WEBOS_GLIBC_FN char *__secure_getenv(const char *name);
#define secure_getenv __secure_getenv
/* NSS builds some C files with a feature set under which glibc 2.12 does
 * not declare putenv(); the function itself is there. */
WEBOS_GLIBC_FN int putenv(char *string);
#endif

#endif
