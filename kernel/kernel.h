// Inversa's Linux kernel layer (see kernel.c)
#ifndef INVERSA_KERNEL_H
#define INVERSA_KERNEL_H
long inv_syscall(long nr, long a, long b, long c, long d, long e, long f);
long inv_gettid(void);
long inv_tkill(long tid, long sig);
void inv_thread_exit(int code) __attribute__((noreturn));
int  inv_sig_to_darwin(int linux_sig);
int  inv_sig_to_linux(int darwin_sig);
const void *inv_guest_action(int linux_sig);       // the guest's struct k_sigaction
void inv_signal_installed(int linux_sig, const void *action);
void *inv_libc_lookup(const char *name);          // what the kernel adds to the guest's libc
void inv_kernel_init(void);
#include <stddef.h>
#include <stdint.h>
long inv_fstatat(long dirfd, const char *path, void *buf, long flags);
long inv_fstat(long fd, void *buf);
long inv_execveat(long dirfd, const char *path, char *const *argv, char *const *envp, long flags);
long inv_openat2(long dirfd, const char *path, const uint64_t *how, size_t size);
extern int inv_x86_mode;                          // running x86-64 code through FEX
#endif
