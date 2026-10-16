// Inversa's Linux kernel layer: the syscall ABI a Linux ARM64 program (or FEX, running an
// x86-64 one) expects, served from Darwin.
#ifndef INVERSA_KERNEL_H
#define INVERSA_KERNEL_H
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

extern int inv_x86_mode;      // running x86-64 code through FEX (memory.c)
extern int inv_arm64_mode;    // running an ARM64 program on its own glibc (linux.c)

// kernel.c
void inv_kernel_init(void);
long inv_syscall(long nr, long a, long b, long c, long d, long e, long f);
void inv_time_mark(const char *what);             // INVERSA_TIME=1: ms since the first exec

// libc.c: what the kernel adds to the guest's libc
void *inv_libc_lookup(const char *name);

// files.c, exec.c, ioctl.c
long inv_fstatat(long dirfd, const char *path, void *buf, long flags);
long inv_fstat(long fd, void *buf);
long inv_openat2(long dirfd, const char *path, const uint64_t *how, size_t size);
long inv_execveat(long dirfd, const char *path, char *const *argv, char *const *envp, long flags);
long inv_ioctl(long fd, long request, long arg);

// memory.c
void *inv_mmap(void *addr, size_t len, long prot, long flags, long fd, long off);
long inv_munmap(void *addr, size_t len);
long inv_mprotect(void *addr, size_t len, long prot);

// threads.c: threads and signals
long inv_gettid(void);
long inv_tkill(long tid, long sig);
void inv_thread_exit(int code) __attribute__((noreturn));
int  inv_sig_to_darwin(int linux_sig);
int  inv_sig_to_linux(int darwin_sig);
const void *inv_guest_action(int linux_sig);       // the guest's struct k_sigaction
void inv_signal_installed(int linux_sig, const void *action);
void inv_signal_after_fork_child(void);

// linux.c: the ARM64 loader (INVERSA_ARM64_LINUX=1)
int  inv_linux_run(const char *path, int argc, char **argv, const char *guest_root);
const char *inv_guest_path(const char *path, char *buf);
long inv_arm64_brk(uintptr_t requested);
void *inv_map_code(void *addr, size_t len, int fixed, int fd, off_t off);
#endif
