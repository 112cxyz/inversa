// Inversa's Linux kernel: the arm64 Linux syscall ABI on Darwin, behind glibc's syscall().
// FEX reaches the kernel only through syscall() (patches/fex-inversa.patch removes its raw
// svc), so every guest x86-64 syscall FEX passes through, and FEX's own, lands here.
//
// Calls whose raw form means the same as the glibc function go to that function as the
// guest's libc serves it (Klepton's and loader/glibc's shims already speak the Linux ABI: flags,
// struct layouts, errno). The rest is written out below. Errors follow glibc: -1 with errno
// set (Darwin's; the guest's errno location translates it).
#include "kernel.h"
#include "syscall_names.h"
#include <errno.h>
#include <fcntl.h>
#include <os/os_sync_wait_on_address.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <mach/mach.h>

void *mvr_glibc_lookup( const char *name );   // loader/glibc
void *kl_shim_lookup( const char *name );     // Klepton
void kl_pthread_atfork_child( void );
void mvr_jit_after_fork_child( void );        // loader/jit.c

// the guest libc as a guest sees it: the glibc layer's overrides, then Klepton's own table
static void *libc_fn( const char *name )
{
    void *p = mvr_glibc_lookup( name );
    return p ? p : kl_shim_lookup( name );
}

typedef long (*sysfn)( long, long, long, long, long, long );

#define NSYS (sizeof linux_syscall_names / sizeof linux_syscall_names[0])

static const char *syscall_name( long nr )
{
    return nr >= 0 && (size_t)nr < NSYS && linux_syscall_names[nr] ? linux_syscall_names[nr] : "?";
}

static int strace = -1;

static long fail( int e ) { errno = e; return -1; }

// INVERSA_T0 is set by the first inversa in the exec chain, so every stage is timed from there
void inv_time_mark( const char *what )
{
    static int on = -1;
    static double t0;
    if (on < 0)
    {
        const char *start = getenv( "INVERSA_T0" );
        on = getenv( "INVERSA_TIME" ) && start;
        if (on) t0 = atof( start );
    }
    if (!on) return;
    struct timespec t;
    clock_gettime( CLOCK_MONOTONIC, &t );
    fprintf( stderr, "inversa: %7.1f ms  %s\n", t.tv_sec * 1e3 + t.tv_nsec / 1e6 - t0, what );
}

// FEX creates CLONE_THREAD workers through its ThreadManager. Its ForkGuest path
// reaches this syscall with SIGCHLD, optionally CLONE_FS/CLONE_FILES, and no
// guest stack: it needs a separate process with a copied address space.
static long sys_clone_process( uint64_t flags, long stack, long parent_tid, long child_tid, long tls )
{
    enum { CLONE_FS = 0x200, CLONE_FILES = 0x400, CLONE_VFORK = 0x4000 };
    if ((flags & 0xff) != 17 || (flags & ~(uint64_t)(CLONE_FS | CLONE_FILES | CLONE_VFORK | 0xff)) ||
        stack || parent_tid || child_tid || tls)
        return fail( ENOSYS );
    pid_t pid = fork();
    if (pid == 0)
    {
        mvr_jit_after_fork_child();
        inv_signal_after_fork_child();
        kl_pthread_atfork_child();   // the child's thread can unlock what it held again
    }
    return pid;
}

static long sys_clone3_process( const uint64_t *args, size_t size )
{
    if (!args || size < 88) return fail( EINVAL );
    // clone3: flags, pidfd, child_tid, parent_tid, exit_signal, stack,
    // stack_size, tls, set_tid, set_tid_size, cgroup.
    if (args[1] || args[2] || args[3] || args[5] || args[6] || args[7] || args[8] || args[9] || args[10])
        return fail( ENOSYS );
    return sys_clone_process( args[0] | args[4], 0, 0, 0, 0 );
}

static int socket_flags( int fd, int flags )
{
    if ((flags & 0x80000) && fcntl( fd, F_SETFD, FD_CLOEXEC ) < 0) return -1;
    if (flags & 0x800)
    {
        int old = fcntl( fd, F_GETFL );
        if (old < 0 || fcntl( fd, F_SETFL, old | O_NONBLOCK ) < 0) return -1;
    }
    return 0;
}

static long sys_socket( long domain, long type, long protocol )
{
    if (type & ~(long)(0x80000 | 0x800 | 0xf)) return fail( EINVAL );
    typedef int (*socket_fn)( int, int, int );
    socket_fn fn = (socket_fn)libc_fn( "socket" );
    if (!fn) return fail( ENOSYS );
    int fd = fn( (int)domain, (int)type & 0xf, (int)protocol );
    if (fd < 0) return -1;
    if (socket_flags( fd, (int)type )) { int e = errno; close( fd ); return fail( e ); }
    return fd;
}

static long sys_accept4( long fd, long addr, long addrlen, long flags )
{
    if (flags & ~(long)(0x80000 | 0x800)) return fail( EINVAL );
    typedef int (*accept_fn)( int, void *, void * );
    accept_fn fn = (accept_fn)libc_fn( "accept" );
    if (!fn) return fail( ENOSYS );
    int client = fn( (int)fd, (void *)addr, (void *)addrlen );
    if (client < 0) return -1;
    if (socket_flags( client, (int)flags )) { int e = errno; close( client ); return fail( e ); }
    return client;
}

/* ---- forwarded by name ----------------------------------------------------- */
// raw syscall -> the libc function with the same meaning and argument order
static const struct { const char *sys, *fn; } same[] = {
    { "read", 0 }, { "write", 0 }, { "close", 0 }, { "lseek", 0 }, { "readv", 0 }, { "writev", 0 },
    { "pread64", "pread" }, { "pwrite64", "pwrite" }, { "dup", 0 }, { "dup3", 0 }, { "pipe2", 0 },
    { "fcntl", 0 }, { "mmap", 0 }, { "munmap", 0 }, { "mprotect", 0 }, { "madvise", 0 }, { "msync", 0 },
    { "mlock", 0 }, { "mkdirat", 0 }, { "unlinkat", 0 }, { "renameat", 0 }, { "symlinkat", 0 },
    { "readlinkat", 0 }, { "faccessat", 0 }, { "fchmodat", 0 }, { "fchownat", 0 }, { "fchmod", 0 },
    { "fchown", 0 }, { "fsync", 0 }, { "fdatasync", 0 }, { "ftruncate", 0 }, { "truncate", 0 },
    { "chdir", 0 }, { "fchdir", 0 }, { "nanosleep", 0 }, { "clock_gettime", 0 }, { "clock_getres", 0 },
    { "gettimeofday", 0 }, { "getpid", 0 }, { "getppid", 0 }, { "getuid", 0 }, { "geteuid", 0 },
    { "getgid", 0 }, { "getegid", 0 }, { "getpgid", 0 }, { "setpgid", 0 }, { "getsid", 0 },
    { "setsid", 0 }, { "umask", 0 }, { "sched_yield", 0 }, { "socketpair", 0 }, { "connect", 0 },
    { "bind", 0 }, { "listen", 0 }, { "accept", 0 }, { "sendto", 0 }, { "recvfrom", 0 },
    { "sendmsg", 0 }, { "recvmsg", 0 }, { "shutdown", 0 }, { "setsockopt", 0 }, { "getsockopt", 0 },
    { "getsockname", 0 }, { "getpeername", 0 }, { "getrusage", 0 }, { "ppoll", 0 },
    { "eventfd2", "eventfd" }, { "utimensat", 0 }, { "fstatfs", 0 }, { "statfs", 0 }, { "fallocate", 0 },
    { "flock", 0 }, { "getxattr", 0 }, { "lgetxattr", 0 }, { "fgetxattr", 0 }, { "listxattr", 0 },
    { "setuid", 0 }, { "setgid", 0 }, { "setreuid", 0 }, { "setregid", 0 }, { "getgroups", 0 },
    { "sched_getscheduler", 0 }, { "sched_getparam", 0 }, { "sched_setparam", 0 },
    { "getpriority", 0 }, { "setpriority", 0 }, { "times", 0 }, { "mincore", 0 },
};

// libc functions returning 64-bit values; every other one returns int, whose -1 must be
// sign-extended (the upper half of x0 is undefined after an int return)
static const char *const wide[] = {
    "read", "write", "readv", "writev", "pread64", "pwrite64", "lseek", "mmap", "sendto", "recvfrom",
    "sendmsg", "recvmsg", "readlinkat", "times", "getxattr", "lgetxattr", "fgetxattr", "listxattr",
};

static sysfn cache[NSYS];
static char looked[NSYS], narrow[NSYS];

static sysfn forwarded( long nr )
{
    if (looked[nr]) return cache[nr];
    looked[nr] = 1;
    const char *name = linux_syscall_names[nr];
    if (!name) return NULL;
    for (size_t i = 0; i < sizeof same / sizeof same[0]; i++)
        if (!strcmp( same[i].sys, name ))
        {
            narrow[nr] = 1;
            for (size_t k = 0; k < sizeof wide / sizeof wide[0]; k++) if (!strcmp( wide[k], name )) narrow[nr] = 0;
            return cache[nr] = (sysfn)libc_fn( same[i].fn ? same[i].fn : name );
        }
    return NULL;
}

/* ---- signals: Linux numbering and layouts --------------------------------- */
// Linux signal n -> Darwin, 0 where Darwin has none (SIGSTKFLT, SIGPWR, real-time)
static const int sig_to_darwin[65] = {
    0, SIGHUP, SIGINT, SIGQUIT, SIGILL, SIGTRAP, SIGABRT, SIGBUS, SIGFPE, SIGKILL, SIGUSR1,
    SIGSEGV, SIGUSR2, SIGPIPE, SIGALRM, SIGTERM, 0, SIGCHLD, SIGCONT, SIGSTOP, SIGTSTP, SIGTTIN,
    SIGTTOU, SIGURG, SIGXCPU, SIGXFSZ, SIGVTALRM, SIGPROF, SIGWINCH, SIGIO, 0, SIGSYS,
    [63] = SIGINFO, // FEX's private thread-pause signal
};

int inv_sig_to_darwin( int s ) { return s > 0 && s <= 64 ? sig_to_darwin[s] : 0; }

int inv_sig_to_linux( int s )
{
    for (int i = 1; i <= 64; i++) if (sig_to_darwin[i] && sig_to_darwin[i] == s) return i;
    return 0;
}

static void set_to_darwin( uint64_t in, sigset_t *out )
{
    sigemptyset( out );
    for (int i = 1; i <= 64; i++)
        if ((in >> (i - 1)) & 1 && sig_to_darwin[i]) sigaddset( out, sig_to_darwin[i] );
}

static uint64_t set_to_linux( const sigset_t *in )
{
    uint64_t out = 0;
    for (int i = 1; i <= 64; i++)
        if (sig_to_darwin[i] && sigismember( in, sig_to_darwin[i] )) out |= 1ull << (i - 1);
    return out;
}

static long sys_pselect6( long nfds, long readfds, long writefds, long exceptfds, long timeout, long sigarg )
{
    struct { const uint64_t *mask; size_t size; } *arg = (void *)sigarg;
    sigset_t mask;
    if (arg)
    {
        if (!arg->mask || arg->size != 8) return fail( EINVAL );
        set_to_darwin( *arg->mask, &mask );
    }
    return pselect( (int)nfds, (fd_set *)readfds, (fd_set *)writefds,
                    (fd_set *)exceptfds, (const struct timespec *)timeout, arg ? &mask : NULL );
}

static long sys_clock_nanosleep( long clock, long flags, long req, long rem )
{
    if (clock != 0 && clock != 1) return fail( EINVAL );
    if (flags & ~1L) return fail( EINVAL );
    const struct timespec *requested = (const struct timespec *)req;
    if (!requested) return fail( EFAULT );
    struct timespec delta = *requested;
    if (flags & 1)
    {
        struct timespec now;
        if (clock_gettime( clock == 0 ? CLOCK_REALTIME : CLOCK_MONOTONIC, &now )) return -1;
        delta.tv_sec -= now.tv_sec;
        delta.tv_nsec -= now.tv_nsec;
        if (delta.tv_nsec < 0) { delta.tv_nsec += 1000000000L; delta.tv_sec--; }
        if (delta.tv_sec < 0) return 0;
    }
    return nanosleep( &delta, flags & 1 ? NULL : (struct timespec *)rem );
}

static long sys_linkat( long olddir, long oldpath, long newdir, long newpath, long flags )
{
    if (flags & ~0x400L) return fail( EINVAL );
    return linkat( olddir == -100 ? AT_FDCWD : (int)olddir, (const char *)oldpath,
                   newdir == -100 ? AT_FDCWD : (int)newdir, (const char *)newpath,
                   flags & 0x400 ? AT_SYMLINK_FOLLOW : 0 );
}

// struct k_sigaction for arm64: handler, flags, restorer, mask
typedef struct { uint64_t handler, flags, restorer, mask; } linux_sigaction;
static linux_sigaction guest_actions[65];
static pthread_mutex_t sig_mu = PTHREAD_MUTEX_INITIALIZER;

static long sys_rt_sigaction( long sig, long act, long old, long size )
{
    if (sig < 1 || sig > 64 || sig == 9 || sig == 19) return fail( EINVAL );
    pthread_mutex_lock( &sig_mu );
    if (old) *(linux_sigaction *)old = guest_actions[sig];
    if (act) guest_actions[sig] = *(const linux_sigaction *)act;
    pthread_mutex_unlock( &sig_mu );
    if (act) inv_signal_installed( (int)sig, &guest_actions[sig] );
    return 0;
}

const void *inv_guest_action( int sig ) { return sig > 0 && sig <= 64 ? &guest_actions[sig] : NULL; }

static long sys_rt_sigprocmask( long how, long set, long old, long size )
{
    sigset_t s, o;
    if (set) set_to_darwin( *(const uint64_t *)set, &s );
    int dhow = how == 0 ? SIG_BLOCK : how == 1 ? SIG_UNBLOCK : SIG_SETMASK;
    int r = pthread_sigmask( dhow, set ? &s : NULL, &o );
    if (r) return fail( r );
    if (old) *(uint64_t *)old = set_to_linux( &o );
    return 0;
}

// Linux stack_t is { sp, int flags, size }; Darwin's is { sp, size, int flags }
typedef struct { uint64_t sp; int32_t flags; int32_t pad; uint64_t size; } linux_stack;

static long sys_sigaltstack( long ss, long old )
{
    stack_t d, o;
    if (ss)
    {
        const linux_stack *l = (const linux_stack *)ss;
        d.ss_sp = (void *)l->sp; d.ss_size = l->size; d.ss_flags = (l->flags & 2) ? SS_DISABLE : 0;
    }
    if (sigaltstack( ss ? &d : NULL, &o )) return -1;
    if (old)
    {
        linux_stack *l = (linux_stack *)old;
        l->sp = (uint64_t)o.ss_sp; l->size = o.ss_size; l->pad = 0;
        l->flags = (o.ss_flags & SS_ONSTACK ? 1 : 0) | (o.ss_flags & SS_DISABLE ? 2 : 0);
    }
    return 0;
}

/* ---- threads and futexes -------------------------------------------------- */
long inv_gettid(void) { return (long)pthread_mach_thread_np( pthread_self() ); }

static long sys_futex( long uaddr, long op, long val, long timeout, long uaddr2, long val3 )
{
    int cmd = op & 0x7f;   // without FUTEX_PRIVATE_FLAG (128) and FUTEX_CLOCK_REALTIME (256)
    void *addr = (void *)uaddr;
    switch (cmd)
    {
    case 0: case 9: {      // FUTEX_WAIT, FUTEX_WAIT_BITSET
        int r;
        if (timeout)
        {
            const struct timespec *ts = (const struct timespec *)timeout;
            uint64_t ns = (uint64_t)ts->tv_sec * 1000000000ull + ts->tv_nsec;
            if (cmd == 9)  // absolute
            {
                struct timespec now;
                clock_gettime( (op & 256) ? CLOCK_REALTIME : CLOCK_MONOTONIC, &now );
                uint64_t n = (uint64_t)now.tv_sec * 1000000000ull + now.tv_nsec;
                ns = ns > n ? ns - n : 0;
                if (!ns) return fail( ETIMEDOUT );
            }
            r = os_sync_wait_on_address_with_timeout( addr, (uint32_t)val, 4, OS_SYNC_WAIT_ON_ADDRESS_NONE,
                                                       OS_CLOCK_MACH_ABSOLUTE_TIME, ns * 3 / 125 );   // Apple silicon: 125/3 ns a tick
        }
        else r = os_sync_wait_on_address( addr, (uint32_t)val, 4, OS_SYNC_WAIT_ON_ADDRESS_NONE );
        if (r < 0 && errno == EFAULT) return -1;
        if (r < 0 && errno == ETIMEDOUT) return -1;
        if (r < 0 && errno == EINTR) return -1;
        return 0;   // woken, or the value had already changed (Linux says EAGAIN; callers retry either way)
    }
    case 1: case 10: {     // FUTEX_WAKE, FUTEX_WAKE_BITSET
        if (val >= 0x7fffffff) return os_sync_wake_by_address_all( addr, 4, OS_SYNC_WAKE_BY_ADDRESS_NONE ) ? 0 : 1;
        long n = 0;
        for (; n < val; n++)
            if (os_sync_wake_by_address_any( addr, 4, OS_SYNC_WAKE_BY_ADDRESS_NONE )) break;
        return n;
    }
    // Priority-inheritance mutexes (glibc's PTHREAD_PRIO_INHERIT; Unity's Mono uses them): the word
    // is the owner's tid, with FUTEX_WAITERS (bit 31) set once someone waits. macOS has no
    // priority inheritance, so these are a plain lock on the word: an unlock clears it and wakes
    // every waiter, and each tries again, setting the waiters bit before it sleeps.
    case 6: case 13: {     // FUTEX_LOCK_PI, FUTEX_LOCK_PI2
        uint32_t *w = addr, tid = (uint32_t)inv_gettid();
        uint64_t deadline = 0;
        int clock = cmd == 6 || (op & 256) ? CLOCK_REALTIME : CLOCK_MONOTONIC;
        if (timeout)
        {
            const struct timespec *ts = (const struct timespec *)timeout;
            deadline = (uint64_t)ts->tv_sec * 1000000000ull + ts->tv_nsec;
        }
        for (;;)
        {
            uint32_t cur = __atomic_load_n( w, __ATOMIC_ACQUIRE );
            if (!(cur & 0x3fffffff))
            {
                if (__atomic_compare_exchange_n( w, &cur, tid, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED )) return 0;
                continue;
            }
            if ((cur & 0x3fffffff) == tid) return fail( EDEADLK );
            if (!(cur & 0x80000000))
            {
                if (!__atomic_compare_exchange_n( w, &cur, cur | 0x80000000, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED )) continue;
                cur |= 0x80000000;
            }
            int r;
            if (timeout)
            {
                struct timespec now;
                clock_gettime( clock, &now );
                uint64_t n = (uint64_t)now.tv_sec * 1000000000ull + now.tv_nsec;
                if (n >= deadline) return fail( ETIMEDOUT );
                r = os_sync_wait_on_address_with_timeout( addr, cur, 4, OS_SYNC_WAIT_ON_ADDRESS_NONE,
                                                           OS_CLOCK_MACH_ABSOLUTE_TIME, (deadline - n) * 3 / 125 );
            }
            else r = os_sync_wait_on_address( addr, cur, 4, OS_SYNC_WAIT_ON_ADDRESS_NONE );
            if (r < 0 && errno == EFAULT) return -1;
        }
    }
    case 7: {              // FUTEX_UNLOCK_PI
        uint32_t *w = addr, tid = (uint32_t)inv_gettid();
        uint32_t cur = __atomic_load_n( w, __ATOMIC_RELAXED );
        if ((cur & 0x3fffffff) != tid) return fail( EPERM );
        cur = __atomic_exchange_n( w, 0, __ATOMIC_RELEASE );
        if (cur & 0x80000000) os_sync_wake_by_address_all( addr, 4, OS_SYNC_WAKE_BY_ADDRESS_NONE );
        return 0;
    }
    case 8: {              // FUTEX_TRYLOCK_PI
        uint32_t *w = addr, tid = (uint32_t)inv_gettid(), cur = 0;
        if (__atomic_compare_exchange_n( w, &cur, tid, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED )) return 0;
        return fail( (cur & 0x3fffffff) == tid ? EDEADLK : EAGAIN );
    }
    }
    if (strace) fprintf( stderr, "inversa: futex op %ld not implemented\n", op );
    return fail( ENOSYS );
}

/* ---- system ---------------------------------------------------------------- */
// Linux's struct sysinfo on 64-bit. Mesa's llvmpipe sizes itself from totalram.
struct linux_sysinfo {
    long uptime;
    unsigned long loads[3], totalram, freeram, sharedram, bufferram, totalswap, freeswap;
    unsigned short procs, pad;
    unsigned long totalhigh, freehigh;
    unsigned int mem_unit;
    char reserved[4];
};

static long sys_sysinfo( long out )
{
    struct linux_sysinfo si = {0};
    struct timeval boot; size_t len = sizeof boot;
    if (!sysctlbyname( "kern.boottime", &boot, &len, NULL, 0 )) si.uptime = time( NULL ) - boot.tv_sec;
    uint64_t mem = 0; len = sizeof mem;
    sysctlbyname( "hw.memsize", &mem, &len, NULL, 0 );
    si.totalram = mem;
    vm_statistics64_data_t vm; mach_msg_type_number_t n = HOST_VM_INFO64_COUNT;
    if (host_statistics64( mach_host_self(), HOST_VM_INFO64, (host_info64_t)&vm, &n ) == KERN_SUCCESS)
        si.freeram = (uint64_t)(vm.free_count + vm.inactive_count) * vm_page_size;
    double load[3];
    if (getloadavg( load, 3 ) == 3)
        for (int i = 0; i < 3; i++) si.loads[i] = (unsigned long)(load[i] * 65536.0);
    si.procs = 1;
    si.mem_unit = 1;
    memcpy( (void *)out, &si, sizeof si );
    return 0;
}

// memfd_create: an unlinked file, which resizes and maps like one (sealing isn't kept)
static long sys_memfd_create( long name, long flags )
{
    (void)name;
    const char *dir = getenv( "TMPDIR" );
    char path[1024];
    snprintf( path, sizeof path, "%s/inversa-memfd-XXXXXX", dir && *dir ? dir : "/tmp" );
    int fd = mkstemp( path );
    if (fd < 0) return -1;
    unlink( path );
    if (flags & 1) fcntl( fd, F_SETFD, FD_CLOEXEC );   // MFD_CLOEXEC
    return fd;
}

/* ---- files ----------------------------------------------------------------- */
// futimens(fd, times) is utimensat(fd, NULL, times, 0) on Linux; Darwin's utimensat wants a path
// (EFAULT, which AppImages unpacking themselves hit on every file), so that form is futimens.
// Linux's UTIME_NOW/UTIME_OMIT are 2^30-1 and 2^30-2, Darwin's -1 and -2.
static long sys_futimens( long fd, long times )
{
    struct timespec ts[2];
    if (!times) return futimens( (int)fd, NULL );
    memcpy( ts, (const void *)times, sizeof ts );
    for (int i = 0; i < 2; i++)
    {
        if (ts[i].tv_nsec == (1l << 30) - 1) ts[i].tv_nsec = UTIME_NOW;
        else if (ts[i].tv_nsec == (1l << 30) - 2) ts[i].tv_nsec = UTIME_OMIT;
    }
    return futimens( (int)fd, ts );
}

// Darwin's getdirentries64 records -> Linux dirent64
static long sys_getdents64( long fd, long buf, long count )
{
    char tmp[8192];
    long basep = 0, in = syscall( SYS_getdirentries64, (int)fd, tmp, count < (long)sizeof tmp ? count : (long)sizeof tmp, &basep );
    if (in <= 0) return in;
    long out = 0;
    for (long p = 0; p < in;)
    {
        struct { uint64_t ino, seekoff; uint16_t reclen, namlen; uint8_t type; char name[]; } *d = (void *)(tmp + p);
        long rec = (19 + d->namlen + 1 + 7) & ~7;
        if (out + rec > count)
        {
            lseek( (int)fd, (off_t)d->seekoff, SEEK_SET );   // the rest on the next call
            break;
        }
        char *o = (char *)buf + out;
        *(uint64_t *)o = d->ino; *(int64_t *)(o + 8) = (int64_t)d->seekoff;
        *(uint16_t *)(o + 16) = (uint16_t)rec; o[18] = (char)d->type;
        memcpy( o + 19, d->name, d->namlen ); o[19 + d->namlen] = 0;
        out += rec; p += d->reclen;
    }
    return out;
}

// struct statx from Darwin's fstatat. Linux AT_ flags: SYMLINK_NOFOLLOW 0x100, NO_AUTOMOUNT 0x800
// (nothing to do), EMPTY_PATH 0x1000; the sync-type bits are hints
static long sys_statx( long dirfd, long path, long flags, long mask, long out )
{
    struct stat st;
    int fd = (int)dirfd == -100 ? AT_FDCWD : (int)dirfd;
    const char *p = (const char *)path;
    int r = (flags & 0x1000) && (!p || !*p) ? fstat( fd, &st )
                                            : fstatat( fd, p, &st, (flags & 0x100) ? AT_SYMLINK_NOFOLLOW : 0 );
    if (r) return -1;
    uint8_t *x = (uint8_t *)out;
    memset( x, 0, 256 );
    *(uint32_t *)(x + 0) = 0x7ff;            // stx_mask: STATX_BASIC_STATS
    *(uint32_t *)(x + 4) = (uint32_t)st.st_blksize;
    *(uint32_t *)(x + 16) = st.st_nlink; *(uint32_t *)(x + 20) = st.st_uid; *(uint32_t *)(x + 24) = st.st_gid;
    *(uint16_t *)(x + 28) = st.st_mode;
    *(uint64_t *)(x + 32) = st.st_ino; *(uint64_t *)(x + 40) = (uint64_t)st.st_size; *(uint64_t *)(x + 48) = (uint64_t)st.st_blocks;
    const struct timespec *t[4] = { &st.st_atimespec, &st.st_birthtimespec, &st.st_ctimespec, &st.st_mtimespec };
    for (int i = 0; i < 4; i++) { *(int64_t *)(x + 64 + i * 16) = t[i]->tv_sec; *(uint32_t *)(x + 72 + i * 16) = (uint32_t)t[i]->tv_nsec; }
    *(uint32_t *)(x + 0) |= 0x800;           // STATX_BTIME: Darwin has it
    *(uint32_t *)(x + 128) = (uint32_t)major( st.st_rdev ); *(uint32_t *)(x + 132) = (uint32_t)minor( st.st_rdev );
    *(uint32_t *)(x + 136) = (uint32_t)major( st.st_dev ); *(uint32_t *)(x + 140) = (uint32_t)minor( st.st_dev );
    return 0;
}

/* ---- the system -------------------------------------------------------------- */
static long sys_uname( long buf )
{
    char *u = (char *)buf;
    memset( u, 0, 6 * 65 );
    strcpy( u, "Linux" );
    gethostname( u + 65, 64 );
    strcpy( u + 130, "6.8.0-inversa" );
    strcpy( u + 195, "#1 SMP Inversa" );
    strcpy( u + 260, "aarch64" );
    strcpy( u + 325, "(none)" );
    return 0;
}

static int rlimit_to_darwin( long r )
{
    switch (r)
    {
    case 0: return RLIMIT_CPU; case 1: return RLIMIT_FSIZE; case 2: return RLIMIT_DATA;
    case 3: return RLIMIT_STACK; case 4: return RLIMIT_CORE; case 5: return RLIMIT_RSS;
    case 6: return RLIMIT_NPROC; case 7: return RLIMIT_NOFILE; case 8: return RLIMIT_MEMLOCK;
    case 9: return RLIMIT_AS;
    }
    return -1;
}

static long sys_prlimit64( long pid, long res, long nl, long ol )
{
    if (pid && pid != getpid()) return fail( EPERM );
    int r = rlimit_to_darwin( res );
    if (r < 0)   // Linux-only limits: unlimited
    {
        if (ol) ((uint64_t *)ol)[0] = ((uint64_t *)ol)[1] = ~0ull;
        return 0;
    }
    struct rlimit cur;
    if (getrlimit( r, &cur )) return -1;
    if (ol)
    {
        ((uint64_t *)ol)[0] = cur.rlim_cur == RLIM_INFINITY ? ~0ull : cur.rlim_cur;
        ((uint64_t *)ol)[1] = cur.rlim_max == RLIM_INFINITY ? ~0ull : cur.rlim_max;
    }
    if (nl)
    {
        struct rlimit n = { ((uint64_t *)nl)[0], ((uint64_t *)nl)[1] };
        if (n.rlim_cur == ~0ull) n.rlim_cur = RLIM_INFINITY;
        if (n.rlim_max == ~0ull) n.rlim_max = RLIM_INFINITY;
        if (setrlimit( r, &n )) return -1;
    }
    return 0;
}

static long sys_sched_getaffinity( long pid, long size, long mask )
{
    int n = 0; size_t len = sizeof n;
    sysctlbyname( "hw.ncpu", &n, &len, NULL, 0 );
    if (size < 8) return fail( EINVAL );
    memset( (void *)mask, 0, (size_t)size );
    for (int i = 0; i < n && i < size * 8; i++) ((uint8_t *)mask)[i / 8] |= 1 << (i % 8);
    return 8;
}

static long sys_prctl( long op, long a, long b, long c, long d )
{
    switch (op)
    {
    case 15: pthread_setname_np( (const char *)a ); return 0;                       // PR_SET_NAME
    case 16: pthread_getname_np( pthread_self(), (char *)a, 16 ); return 0;        // PR_GET_NAME
    case 1: case 4: case 38: case 0x59616d61: return 0;   // PDEATHSIG, SET_DUMPABLE, NO_NEW_PRIVS, SET_PTRACER
    case 3: return 1;                                       // PR_GET_DUMPABLE
    }
    return fail( EINVAL );
}

// grow, shrink or move a private anonymous-style mapping; contents copied when it moves
static long sys_mremap( long old, long oldsz, long newsz, long flags, long newaddr )
{
    if (newsz <= oldsz)
    {
        if (newsz < oldsz) munmap( (char *)old + newsz, (size_t)(oldsz - newsz) );
        return old;
    }
    if (!(flags & 1)) return fail( ENOMEM );   // MREMAP_MAYMOVE
    void *p = mmap( (flags & 2) ? (void *)newaddr : NULL, (size_t)newsz, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANON | ((flags & 2) ? MAP_FIXED : 0), -1, 0 );
    if (p == MAP_FAILED) return -1;
    memcpy( p, (void *)old, (size_t)oldsz );
    munmap( (void *)old, (size_t)oldsz );
    return (long)p;
}

/* ---- dispatch ------------------------------------------------------------------ */
static long sys_openat( long dirfd, long path, long flags, long mode )
{
    // the glibc layer's openat translates the flags but takes Darwin's AT_FDCWD (-2, not -100)
    typedef int (*openat_fn)( int, const char *, int, int );
    openat_fn fn = (openat_fn)libc_fn( "openat" );
    if (!fn) return fail( ENOSYS );
    return fn( (int)dirfd == -100 ? AT_FDCWD : (int)dirfd, (const char *)path, (int)flags, (int)mode );
}

static long sys_wait4( long pid, long status, long options, long rusage )
{
    // Linux WCONTINUED is 8, Darwin's 16; the status word and struct rusage match on arm64
    if (options & ~11L) return fail( EINVAL );
    int o = (int)(options & 3) | ((options & 8) ? WCONTINUED : 0);
    return wait4( (pid_t)pid, (int *)status, o, (struct rusage *)rusage );
}

static long dispatch( long nr, long a, long b, long c, long d, long e, long f )
{
    switch (nr)
    {
    case NR_getcwd: return getcwd( (char *)a, (size_t)b ) ? (long)strlen( (char *)a ) + 1 : -1;
    case NR_ioctl: return inv_ioctl( a, b, c );
    case NR_linkat: return sys_linkat( a, b, c, d, e );
    case NR_openat: return sys_openat( a, b, c, d );
    case NR_getdents64: return sys_getdents64( a, b, c );
    case NR_pselect6: return sys_pselect6( a, b, c, d, e, f );
    case NR_newfstatat: return inv_fstatat( a, (const char *)b, (void *)c, d );
    case NR_fstat: return inv_fstat( a, (void *)b );
    case NR_exit: inv_thread_exit( (int)a );
    case NR_exit_group: inv_time_mark( "exit_group" ); _exit( (int)a );
    case NR_set_tid_address: return inv_gettid();
    case NR_futex: return sys_futex( a, b, c, d, e, f );
    case NR_set_robust_list: return fail( ENOSYS );
    case NR_clock_nanosleep: return sys_clock_nanosleep( a, b, c, d );
    case NR_sched_setaffinity: return 0;
    // macOS has no real-time policies to give: Linux's priority ranges, and a requested policy
    // is taken as given (Mono aborts when it can't set a thread's priority)
    case NR_sched_get_priority_max: return a == 1 || a == 2 ? 99 : 0;
    case NR_sched_get_priority_min: return a == 1 || a == 2 ? 1 : 0;
    case NR_sched_setscheduler: return 0;
    case NR_sched_getaffinity: return sys_sched_getaffinity( a, b, c );
    case NR_kill: return kill( (pid_t)a, inv_sig_to_darwin( (int)b ) );
    case NR_tkill: return inv_tkill( a, b );
    case NR_tgkill: return inv_tkill( b, c );
    case NR_sigaltstack: return sys_sigaltstack( a, b );
    case NR_rt_sigaction: return sys_rt_sigaction( a, b, c, d );
    case NR_rt_sigprocmask: return sys_rt_sigprocmask( a, b, c, d );
    case NR_uname: return sys_uname( a );
    case NR_prctl: return sys_prctl( a, b, c, d, e );
    case NR_getcpu: if (a) *(uint32_t *)a = 0; if (b) *(uint32_t *)b = 0; return 0;
    case NR_gettid: return inv_gettid();
    case NR_socket: return sys_socket( a, b, c );
    case NR_brk: return inv_arm64_mode ? inv_arm64_brk( (uintptr_t)a ) : 0;   // FEX keeps the guest's break itself
    case NR_mremap: return sys_mremap( a, b, c, d, e );
    case NR_clone: return sys_clone_process( (uint64_t)a, b, c, d, e );
    case NR_execve: return inv_execveat( -100, (const char *)a, (char *const *)b, (char *const *)c, 0 );
    case NR_fadvise64: return 0;
    case NR_sysinfo: return sys_sysinfo( a );
    case NR_memfd_create: return sys_memfd_create( a, b );
    case NR_utimensat: if (!b) return sys_futimens( a, c ); break;
    case NR_accept4: return sys_accept4( a, b, c, d );
    case NR_wait4: return sys_wait4( a, b, c, d );
    case NR_prlimit64: return sys_prlimit64( a, b, c, d );
    case NR_getrandom: arc4random_buf( (void *)a, (size_t)b ); return b;
    case NR_membarrier: return fail( ENOSYS );
    case NR_execveat: return inv_execveat( a, (const char *)b, (char *const *)c, (char *const *)d, e );
    case NR_statx: return sys_statx( a, b, c, d, e );
    case NR_rseq: return fail( ENOSYS );
    case NR_clone3: return sys_clone3_process( (const uint64_t *)a, (size_t)b );
    case NR_openat2: return inv_openat2( a, (const char *)b, (const uint64_t *)c, (size_t)d );
    }
    sysfn fn = nr >= 0 && (size_t)nr < NSYS ? forwarded( nr ) : NULL;
    if (fn)
    {
        long r = fn( a, b, c, d, e, f );
        return narrow[nr] ? (long)(int)r : r;
    }
    static char warned[1024];
    if (nr >= 0 && nr < 1024 && !warned[nr])
    {
        warned[nr] = 1;
        fprintf( stderr, "inversa: syscall %ld (%s) not implemented\n", nr, syscall_name( nr ) );
    }
    return fail( ENOSYS );
}

// which argument is a path name: 1 for most (after a directory fd), 0 for a few, -1 for none
static int path_arg( long nr )
{
    switch (nr)
    {
    case NR_openat: case NR_newfstatat: case NR_readlinkat: case NR_faccessat: case NR_faccessat2:
    case NR_statx: case NR_unlinkat: case NR_mkdirat: case NR_openat2: case NR_execveat:
        return 1;
    case NR_chdir: case NR_execve:
        return 0;
    }
    return -1;
}

// INVERSA_STRACE=names: which forwarded calls have no libc function behind them
static void check_names(void)
{
    for (size_t i = 0; i < sizeof same / sizeof same[0]; i++)
        if (!libc_fn( same[i].fn ? same[i].fn : same[i].sys ))
            fprintf( stderr, "inversa: %s -> %s: no libc function\n", same[i].sys, same[i].fn ? same[i].fn : same[i].sys );
}

long inv_syscall( long nr, long a, long b, long c, long d, long e, long f )
{
    if (strace < 0)
    {
        const char *opt = getenv( "INVERSA_STRACE" );
        strace = opt && *opt != '0';
        if (opt && !strcmp( opt, "names" )) check_names();
    }
    long args[6] = { a, b, c, d, e, f };
    int p = path_arg( nr );
    char rooted[4096];
    if (inv_arm64_mode && p >= 0) args[p] = (long)inv_guest_path( (const char *)args[p], rooted );
    long r = dispatch( nr, args[0], args[1], args[2], args[3], args[4], args[5] );
    if (strace)
    {
        int err = errno;
        if (p == 1) fprintf( stderr, "[inv] %s(%d, \"%s\", %#lx, %#lx) = %ld", syscall_name( nr ), (int)args[0], (const char *)args[1], args[2], args[3], r );
        else if (p == 0) fprintf( stderr, "[inv] %s(\"%s\", %#lx, %#lx) = %ld", syscall_name( nr ), (const char *)args[0], args[1], args[2], r );
        else fprintf( stderr, "[inv] %s(%#lx, %#lx, %#lx, %#lx) = %ld", syscall_name( nr ), args[0], args[1], args[2], args[3], r );
        if (r == -1) fprintf( stderr, " (%s)", strerror( err ) );
        fputc( '\n', stderr );
        errno = err;
    }
    return r;
}
