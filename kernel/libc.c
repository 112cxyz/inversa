// The guest libc functions Inversa adds: syscall() itself, and the ones FEX imports that the
// glibc layer below doesn't serve.
#include "kernel.h"
#include "syscall_names.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/xattr.h>
#include <stdlib.h>
#include <unistd.h>

void *mvr_glibc_lookup( const char *name );
extern void *(*mvr_glibc_extra)( const char *name );

static long enosys(void) { errno = ENOSYS; return -1; }
static long eperm(void) { errno = EPERM; return -1; }
static long zero(void) { return 0; }

static long g_statx( long dirfd, const char *path, long flags, long mask, void *buf )
{
    return inv_syscall( NR_statx, dirfd, (long)path, flags, mask, (long)buf, 0 );
}
// Klepton's dup2 refuses stdout/stderr as targets (an Android-app guard); a Linux program
// redirecting into a pipe needs exactly that
static long g_dup2( long from, long to ) { return dup2( (int)from, (int)to ); }
static long g_dup3( long from, long to, long flags )
{
    if (from == to || (flags & ~0x80000L)) { errno = EINVAL; return -1; }
    int r = dup2( (int)from, (int)to );
    if (r >= 0 && (flags & 0x80000)) fcntl( r, F_SETFD, FD_CLOEXEC );
    return r;
}
static long g_execve( const char *path, char *const *argv, char *const *envp ) { return inv_execveat( -100, path, argv, envp, 0 ); }
static long g_uname( void *buf ) { return inv_syscall( NR_uname, (long)buf, 0, 0, 0, 0, 0 ); }
static long g_sigaltstack( const void *ss, void *old ) { return inv_syscall( NR_sigaltstack, (long)ss, (long)old, 0, 0, 0, 0 ); }
static long g_getdents64( long fd, long buf, long n ) { return inv_syscall( NR_getdents64, fd, buf, n, 0, 0, 0 ); }
static long g_tgkill( long pid, long tid, long sig ) { return inv_tkill( tid, sig ); }
static long g_getcpu( unsigned *cpu, unsigned *node ) { if (cpu) *cpu = 0; if (node) *node = 0; return 0; }
static long g_creat( const char *path, long mode )
{
    typedef int (*open_fn)( const char *, int, int );
    return ((open_fn)mvr_glibc_lookup( "open" ))( path, 01 | 0100 | 01000, (int)mode );   // O_WRONLY|O_CREAT|O_TRUNC
}
static long g_shm_open( const char *name, long flags, long mode )
{
    // Linux O_ flags (as the guest gives them) -> Darwin
    int f = (flags & 3) | (flags & 0100 ? O_CREAT : 0) | (flags & 0200 ? O_EXCL : 0) | (flags & 01000 ? O_TRUNC : 0);
    return shm_open( name, f, (mode_t)mode );
}
static long g_lremovexattr( const char *p, const char *n ) { return removexattr( p, n, XATTR_NOFOLLOW ); }
static long g_munlock( void *a, size_t n ) { return munlock( a, n ); }
static long g_attr_getdetachstate( const void *attr, int *state ) { *state = 0; return 0; }   // JOINABLE
static long g___overflow( void *f, long ch )
{
    typedef int (*fputc_fn)( int, void * );
    return ((fputc_fn)mvr_glibc_lookup( "fputc" ))( (int)ch, f );
}
static long g_fflush_unlocked( void *f )
{
    typedef int (*fflush_fn)( void * );
    return ((fflush_fn)mvr_glibc_lookup( "fflush" ))( f );
}

static const struct { const char *name; void *fn; } table[] = {
    { "syscall", inv_syscall }, { "mmap", inv_mmap }, { "mmap64", inv_mmap }, { "munmap", inv_munmap },
    { "mprotect", inv_mprotect },
    { "dup2", g_dup2 }, { "dup3", g_dup3 }, { "execve", g_execve },
    { "uname", g_uname }, { "statx", g_statx },
    { "fstatat", inv_fstatat }, { "fstatat64", inv_fstatat }, { "fstat", inv_fstat }, { "fstat64", inv_fstat },
    { "sigaltstack", g_sigaltstack }, { "getdents64", g_getdents64 }, { "tgkill", g_tgkill }, { "getcpu", g_getcpu },
    { "creat", g_creat }, { "creat64", g_creat }, { "shm_open", g_shm_open }, { "shm_unlink", shm_unlink },
    { "lremovexattr", g_lremovexattr }, { "munlock", g_munlock },
    { "pthread_attr_getdetachstate", g_attr_getdetachstate },
    { "__overflow", g___overflow }, { "fflush_unlocked", g_fflush_unlocked },
    { "personality", zero },
    { "settimeofday", eperm }, { "clock_settime", eperm }, { "adjtimex", eperm }, { "clock_adjtime", eperm },
    { "signalfd", enosys }, { "timerfd_settime", enosys }, { "timerfd_gettime", enosys },
    { "process_vm_readv", enosys }, { "process_vm_writev", enosys }, { "vmsplice", enosys },
    { "sched_rr_get_interval", enosys },
};

void *inv_libc_lookup( const char *name )
{
    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++)
        if (!strcmp( table[i].name, name )) return table[i].fn;
    return NULL;
}

void inv_kernel_init(void) { mvr_glibc_extra = inv_libc_lookup; }
