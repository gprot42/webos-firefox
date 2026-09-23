/* Linked into every Firefox binary alongside the static libstdc++. All
 * symbols are hidden, so they never replace the TV's own copies for other
 * libraries in the process (the Mali driver uses the TV's libstdc++). */

/* libstdc++.a from GCC 11 on Debian 12 was built against glibc 2.36 and reads
 * __libc_single_threaded (glibc 2.32+) to skip atomics in single-threaded
 * programs. The TVs' glibc may be older. 0 means "may be multi-threaded",
 * which is always correct: libstdc++ just keeps using atomic reference
 * counts. */
__attribute__((visibility("hidden"))) char __libc_single_threaded = 0;

/* Debian armel's libstdc++.a is compiled for ARMv5, which has no atomic
 * instructions, so it calls three __sync_* helpers. Without these, libgcc.a's
 * linux-atomic.o would supply them, and in libxul it clashes with the copies
 * in Rust's compiler_builtins ("duplicate symbol"). These use ARMv7's own
 * LDREX/STREX and DMB. They are weak, so a strong copy from Rust or libgcc,
 * if one is ever pulled in for another reason, wins without a clash. The
 * names are set with asm labels because C forbids defining builtins. */
#define HELPER __attribute__((visibility("hidden"), weak))

HELPER void webos_sync_synchronize(void) __asm__("__sync_synchronize");
void webos_sync_synchronize(void)
{
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

/* __sync_* operations are full barriers, hence SEQ_CST. */
HELPER unsigned int webos_sync_val_cas_4(volatile void *ptr, unsigned int expected,
                                         unsigned int desired)
    __asm__("__sync_val_compare_and_swap_4");
unsigned int webos_sync_val_cas_4(volatile void *ptr, unsigned int expected,
                                  unsigned int desired)
{
    __atomic_compare_exchange_n((volatile unsigned int *)ptr, &expected, desired, 0,
                                __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return expected;
}

HELPER unsigned int webos_sync_fetch_add_4(volatile void *ptr, unsigned int value)
    __asm__("__sync_fetch_and_add_4");
unsigned int webos_sync_fetch_add_4(volatile void *ptr, unsigned int value)
{
    return __atomic_fetch_add((volatile unsigned int *)ptr, value, __ATOMIC_SEQ_CST);
}
