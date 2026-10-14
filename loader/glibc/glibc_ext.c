// The rest of glibc's surface for Steam Linux Runtime games: glibc-only spellings
// of standard functions, the places where glibc's ABI differs from Darwin's, and
// Linux-only calls that report ENOSYS so callers take their fallback path.
#include "glibc.h"
#include "klepton.h"
#include "kl_va.h"
#include <ctype.h>
#include <dirent.h>
#include <libgen.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <langinfo.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mount.h>
#include <mach/mach.h>
#include <mach/vm_map.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/sem.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysctl.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>
#include <xlocale.h>

#define LINUX_ENOSYS 38
#define LINUX_ENOTSUP 95

/* Klepton's own answer for a name, bypassing our override (which would hand back
 * our wrapper and recurse). Cached: wrappers sit on hot paths like write(). */
static void *klepton( const char *name )
{
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    static struct { const char *name; void *fn; } cache[64];
    static int n;
    pthread_mutex_lock( &lock );
    for (int i = 0; i < n; i++)
        if (!strcmp( cache[i].name, name )) { void *f = cache[i].fn; pthread_mutex_unlock( &lock ); return f; }
    void *(*saved)( const char * ) = kl_shim_override;
    kl_shim_override = NULL;
    void *f = kl_shim_lookup( name );
    kl_shim_override = saved;
    if (n < 64) { cache[n].name = strdup( name ); cache[n].fn = f; n++; }
    if (getenv( "MVR_TRACE" )) fprintf( stderr, "[mvr] klepton(%s) = %p\n", name, f );
    pthread_mutex_unlock( &lock );
    return f;
}
static long set_errno( int darwin_e ) { errno = darwin_e; return -1; }
static long enosys(void) { return set_errno( ENOSYS ); }
static void *enosys_null(void) { errno = ENOSYS; return NULL; }
static long zero(void) { return 0; }

/* ---- /proc/self/exe is the game, not mvr-native ------------------------------ */
static char exe_path[1024];
const char *mvr_glibc_exe_path(void) { return exe_path; }
void mvr_glibc_set_exe( const char *path ) { if (!realpath( path, exe_path )) snprintf( exe_path, sizeof exe_path, "%s", path ); }

static ssize_t proc_link( const char *p, char *buf, size_t n )
{
    if (strcmp( p, "/proc/self/exe" ) && strcmp( p, "/proc/thread-self/exe" )) return -2;
    size_t len = strlen( exe_path );
    if (len > n) len = n;
    memcpy( buf, exe_path, len );
    return (ssize_t)len;
}
static ssize_t g_readlink( const char *p, char *buf, size_t n )
{
    ssize_t r = proc_link( p, buf, n );
    if (r == -2) r = readlink( p, buf, n );
    if (getenv( "MVR_TRACE" )) fprintf( stderr, "[mvr] readlink(%s, %zu) = %zd\n", p, n, r );
    return r;
}
static ssize_t g_readlinkat( int fd, const char *p, char *buf, size_t n )
{
    ssize_t r = proc_link( p, buf, n );
    return r != -2 ? r : readlinkat( fd == -100 ? AT_FDCWD : fd, p, buf, n );
}

/* ---- sysconf: glibc's _SC_ numbering ---------------------------------------- */
static long g_sysconf( int name )
{
    switch (name)
    {
    case 0: return sysconf( _SC_ARG_MAX );
    case 1: return sysconf( _SC_CHILD_MAX );
    case 2: return 100;                                  /* _SC_CLK_TCK, Linux's USER_HZ */
    case 3: return sysconf( _SC_NGROUPS_MAX );
    case 4: return sysconf( _SC_OPEN_MAX );
    case 5: return sysconf( _SC_STREAM_MAX );
    case 6: return sysconf( _SC_TZNAME_MAX );
    case 30: return sysconf( _SC_PAGESIZE );
    case 31: return 32;                                  /* _SC_RTSIG_MAX */
    case 69: return 1024;                                /* _SC_GETGR_R_SIZE_MAX */
    case 70: return 1024;                                /* _SC_GETPW_R_SIZE_MAX */
    case 75: return 16384;                               /* _SC_THREAD_STACK_MIN */
    case 83: return sysconf( _SC_NPROCESSORS_CONF );
    case 84: return sysconf( _SC_NPROCESSORS_ONLN );
    case 85: return sysconf( _SC_PHYS_PAGES );
    case 86: return sysconf( _SC_PHYS_PAGES ) / 2;       /* _SC_AVPHYS_PAGES: no exact Darwin answer */
    case 180: return 255;                                /* _SC_HOST_NAME_MAX */
    case 185: case 188: return 128 * 1024;               /* L1 I/D cache size */
    case 187: case 190: return 64;                       /* L1 line sizes */
    case 191: return 4 * 1024 * 1024;                    /* L2 */
    case 193: return 128;
    case 249: return 5120;                               /* _SC_MINSIGSTKSZ, as arm64 Linux says */
    case 250: return 16384;                              /* _SC_SIGSTKSZ */
    default: errno = EINVAL; return -1;
    }
}

static int g_get_nprocs(void) { return (int)sysconf( _SC_NPROCESSORS_ONLN ); }

/* ---- getauxval: what the kernel would say about an Apple CPU --------------- */
static unsigned long g___getauxval( unsigned long type )
{
    switch (type)
    {
    case 6: return (unsigned long)sysconf( _SC_PAGESIZE );           /* AT_PAGESZ */
    case 16:                                                           /* AT_HWCAP */
        /* fp asimd aes pmull sha1 sha2 crc32 atomics fphp asimdhp asimdrdm jscvt fcma
         * lrcpc dcpop sha3 asimddp sha512 asimdfhm dit uscat ilrcpc flagm ssbs sb */
        return 0x1 | 0x2 | 0x8 | 0x10 | 0x20 | 0x40 | 0x80 | 0x100 | 0x200 | 0x400 |
               0x1000 | 0x2000 | 0x4000 | 0x8000 | 0x10000 | 0x20000 | 0x100000 | 0x200000 |
               0x400000 | 0x800000 | 0x1000000 | 0x2000000 | 0x4000000 | 0x8000000 | 0x10000000 |
               0x20000000;
    case 26: return 0x2 | 0x80 | 0x4000;                              /* AT_HWCAP2: dcpodp, frint, bf16 */
    case 51: return 4096;                                             /* AT_MINSIGSTKSZ */
    case 31: return (unsigned long)exe_path;                          /* AT_EXECFN */
    default: errno = ENOENT; return 0;
    }
}

/* ---- stat family's glibc entry points --------------------------------------- */
typedef int (*stat_fn)( const char *, void * );
typedef int (*fstat_fn)( int, void * );
static int g___xstat( int ver, const char *p, void *b ) { return ((stat_fn)mvr_glibc_lookup( "stat" ))( p, b ); }
static int g___lxstat( int ver, const char *p, void *b ) { return ((stat_fn)mvr_glibc_lookup( "lstat" ))( p, b ); }
static int g___fxstat( int ver, int fd, void *b ) { return ((fstat_fn)mvr_glibc_lookup( "fstat" ))( fd, b ); }
static int g___xmknod( int ver, const char *p, mode_t m, dev_t *d ) { return mknod( p, m, *d ); }

/* struct statfs / statvfs in glibc's aarch64 layout */
struct linux_statfs { int64_t f_type, f_bsize; uint64_t f_blocks, f_bfree, f_bavail, f_files, f_ffree;
                      int32_t f_fsid[2]; int64_t f_namelen, f_frsize, f_flags, f_spare[4]; };
struct linux_statvfs { unsigned long f_bsize, f_frsize; uint64_t f_blocks, f_bfree, f_bavail, f_files,
                       f_ffree, f_favail; unsigned long f_fsid, f_flag, f_namemax; int spare[6]; };

static void fill_statfs( const struct statfs *s, struct linux_statfs *l )
{
    memset( l, 0, sizeof *l );
    l->f_type = 0x58465342;   /* report something mountinfo-shaped: XFS_SUPER_MAGIC */
    l->f_bsize = s->f_bsize; l->f_blocks = s->f_blocks; l->f_bfree = s->f_bfree; l->f_bavail = s->f_bavail;
    l->f_files = s->f_files; l->f_ffree = s->f_ffree; l->f_namelen = 255; l->f_frsize = s->f_bsize;
}
static int g_fstatfs( int fd, struct linux_statfs *l ) { struct statfs s; int r = fstatfs( fd, &s ); if (!r) fill_statfs( &s, l ); return r; }
static int g_statfs( const char *p, struct linux_statfs *l ) { struct statfs s; int r = statfs( p, &s ); if (!r) fill_statfs( &s, l ); return r; }
static void fill_statvfs( const struct statvfs *s, struct linux_statvfs *l )
{
    memset( l, 0, sizeof *l );
    l->f_bsize = s->f_bsize; l->f_frsize = s->f_frsize; l->f_blocks = s->f_blocks; l->f_bfree = s->f_bfree;
    l->f_bavail = s->f_bavail; l->f_files = s->f_files; l->f_ffree = s->f_ffree; l->f_favail = s->f_favail;
    l->f_fsid = s->f_fsid; l->f_flag = s->f_flag & 1; l->f_namemax = s->f_namemax;
}
static int g_statvfs( const char *p, struct linux_statvfs *l ) { struct statvfs s; int r = statvfs( p, &s ); if (!r) fill_statvfs( &s, l ); return r; }
static int g_fstatvfs( int fd, struct linux_statvfs *l ) { struct statvfs s; int r = fstatvfs( fd, &s ); if (!r) fill_statvfs( &s, l ); return r; }

/* *at() calls: Linux AT_FDCWD is -100, and the flag bits differ */
static int at_fd( int fd ) { return fd == -100 ? AT_FDCWD : fd; }
static int at_flags( int f )
{
    return ((f & 0x100) ? AT_SYMLINK_NOFOLLOW : 0) | ((f & 0x200) ? AT_EACCESS : 0) |
           ((f & 0x400) ? AT_SYMLINK_FOLLOW : 0) | ((f & 0x200 && 0) ? AT_REMOVEDIR : 0);
}
static int g_faccessat( int fd, const char *p, int mode, int flags ) { return faccessat( at_fd( fd ), p, mode, at_flags( flags ) ); }
static int g_eaccess( const char *p, int mode ) { return faccessat( AT_FDCWD, p, mode, AT_EACCESS ); }

/* ---- fcntl record locks: Linux struct flock and command/type numbers ---------- */
struct linux_flock { short l_type, l_whence; int64_t l_start, l_len; int l_pid; };
static int lock_type_to_darwin( short t ) { return t == 0 ? F_RDLCK : t == 1 ? F_WRLCK : F_UNLCK; }
static short lock_type_to_linux( short t ) { return t == F_RDLCK ? 0 : t == F_WRLCK ? 1 : 2; }

static int g_fcntl( int fd, int cmd, uintptr_t arg )
{
    int dcmd = -1;
    switch (cmd)
    {
    case 5: case 36: dcmd = F_GETLK; break;      /* F_GETLK, F_OFD_GETLK */
    case 6: case 37: dcmd = F_SETLK; break;      /* F_SETLK, F_OFD_SETLK */
    case 7: case 38: dcmd = F_SETLKW; break;     /* F_SETLKW, F_OFD_SETLKW */
    }
    if (dcmd < 0) return ((int (*)( int, int, uintptr_t ))klepton( "fcntl" ))( fd, cmd, arg );
    struct linux_flock *l = (struct linux_flock *)arg;
    struct flock d = { .l_start = l->l_start, .l_len = l->l_len, .l_pid = l->l_pid,
                       .l_type = lock_type_to_darwin( l->l_type ), .l_whence = l->l_whence };
    int r = fcntl( fd, dcmd, &d );
    if (!r && dcmd == F_GETLK)
    {
        l->l_type = lock_type_to_linux( d.l_type ); l->l_whence = d.l_whence;
        l->l_start = d.l_start; l->l_len = d.l_len; l->l_pid = d.l_pid;
    }
    return r;
}

/* ---- System V semaphores: Linux semctl command numbers, no semtimedop on Darwin */
static int sem_cmd( int c )
{
    switch (c)
    {
    case 0: return IPC_RMID; case 1: return IPC_SET; case 2: return IPC_STAT;
    case 11: return GETPID; case 12: return GETVAL; case 13: return GETALL;
    case 14: return GETNCNT; case 15: return GETZCNT; case 16: return SETVAL; case 17: return SETALL;
    default: return -1;
    }
}

static int g_semget( key_t key, int n, int flags )
{
    int r = semget( key, n, flags );
    if (getenv( "MVR_TRACE" )) fprintf( stderr, "[mvr] semget(%d, %d, %#o) = %d (%s)\n", key, n, flags, r, r < 0 ? strerror( errno ) : "ok" );
    return r;
}

static int g_semctl( int id, int num, int cmd, uintptr_t arg )
{
    int d = sem_cmd( cmd );
    if (d < 0) { errno = EINVAL; return -1; }
    if (d == IPC_STAT || d == IPC_SET) return 0;   /* struct semid_ds differs; nothing in games needs it */
    union semun u;
    if (d == SETVAL) u.val = (int)arg; else u.array = (unsigned short *)arg;
    return semctl( id, num, d, u );
}

static int g_semop( int id, struct sembuf *ops, size_t n ) { return semop( id, ops, n ); }

static int g_semtimedop( int id, struct sembuf *ops, size_t n, const struct timespec *timeout )
{
    if (!timeout) return semop( id, ops, n );
    struct sembuf local[16];
    if (n > 16) { errno = E2BIG; return -1; }
    for (size_t i = 0; i < n; i++) { local[i] = ops[i]; local[i].sem_flg |= IPC_NOWAIT; }
    struct timespec now, end;
    clock_gettime( CLOCK_MONOTONIC, &end );
    end.tv_sec += timeout->tv_sec; end.tv_nsec += timeout->tv_nsec;
    if (end.tv_nsec >= 1000000000) { end.tv_sec++; end.tv_nsec -= 1000000000; }
    for (;;)
    {
        if (!semop( id, local, n )) return 0;
        if (errno != EAGAIN) return -1;
        clock_gettime( CLOCK_MONOTONIC, &now );
        if (now.tv_sec > end.tv_sec || (now.tv_sec == end.tv_sec && now.tv_nsec >= end.tv_nsec)) { errno = EAGAIN; return -1; }
        usleep( 200 );
    }
}

/* ---- eventfd: Klepton's eventfd is a datagram socket carrying one datagram per
 * write, and a unix datagram socket only buffers 4K, so a burst of posts that no one
 * has read yet starts failing and the wakeups are lost (the job system then waits
 * forever). Keep the counter here instead; the socket only holds one token byte,
 * present exactly while the count is non-zero, so poll/epoll still see readiness. */
#define LINUX_EFD_SEMAPHORE 1
#define MAX_EVENTFD 4096
struct eventfd { pthread_mutex_t lock; uint64_t count; unsigned char used, sem; };
static struct eventfd eventfds[MAX_EVENTFD];

static struct eventfd *eventfd_get( int fd )
{
    return fd >= 0 && fd < MAX_EVENTFD && eventfds[fd].used ? &eventfds[fd] : NULL;
}

static void token_drain( int fd )
{
    char c[16];
    while (recv( fd, c, sizeof c, MSG_DONTWAIT ) > 0) ;
}

static int g_eventfd( unsigned int init, int flags )
{
    int fd = ((int (*)( unsigned, int ))klepton( "eventfd" ))( 0, flags & ~LINUX_EFD_SEMAPHORE );
    if (fd < 0) return fd;
    if (fd >= MAX_EVENTFD) return fd;   /* falls back to Klepton's datagram eventfd */
    struct eventfd *e = &eventfds[fd];
    pthread_mutex_init( &e->lock, NULL );
    e->count = init;
    e->sem = !!(flags & LINUX_EFD_SEMAPHORE);
    e->used = 1;
    if (init) send( fd, "", 1, MSG_DONTWAIT );
    return fd;
}

static int eventfd_post( struct eventfd *e, int fd, uint64_t v )
{
    if (v == UINT64_MAX) { errno = EINVAL; return -1; }
    pthread_mutex_lock( &e->lock );
    uint64_t old = e->count;
    e->count += v;
    if (!old && v) send( fd, "", 1, MSG_DONTWAIT );
    pthread_mutex_unlock( &e->lock );
    return 0;
}

static int eventfd_take( struct eventfd *e, int fd, uint64_t *out )
{
    for (;;)
    {
        pthread_mutex_lock( &e->lock );
        if (e->count)
        {
            uint64_t v = e->sem ? 1 : e->count;
            e->count -= v;
            if (!e->count) token_drain( fd );
            pthread_mutex_unlock( &e->lock );
            *out = v;
            return 0;
        }
        pthread_mutex_unlock( &e->lock );
        if (fcntl( fd, F_GETFL ) & O_NONBLOCK) { errno = EAGAIN; return -1; }
        char c;
        if (recv( fd, &c, 1, MSG_PEEK ) < 0 && errno != EINTR) return -1;
    }
}

static ssize_t g_write( int fd, const void *buf, size_t n )
{
    struct eventfd *e = eventfd_get( fd );
    if (e)
    {
        if (n < 8) { errno = EINVAL; return -1; }
        return eventfd_post( e, fd, *(const uint64_t *)buf ) ? -1 : 8;
    }
    return ((ssize_t (*)( int, const void *, size_t ))klepton( "write" ))( fd, buf, n );
}

static ssize_t g_read( int fd, void *buf, size_t n )
{
    struct eventfd *e = eventfd_get( fd );
    if (e)
    {
        uint64_t v;
        if (n < 8) { errno = EINVAL; return -1; }
        if (eventfd_take( e, fd, &v )) return -1;
        memcpy( buf, &v, 8 );
        return 8;
    }
    return ((ssize_t (*)( int, void *, size_t ))klepton( "read" ))( fd, buf, n );
}

static int g_eventfd_write( int fd, uint64_t v )
{
    struct eventfd *e = eventfd_get( fd );
    if (e) return eventfd_post( e, fd, v );
    return ((int (*)( int, uint64_t ))klepton( "eventfd_write" ))( fd, v );
}

static int g_eventfd_read( int fd, uint64_t *v )
{
    struct eventfd *e = eventfd_get( fd );
    if (e) return eventfd_take( e, fd, v );
    return ((int (*)( int, uint64_t * ))klepton( "eventfd_read" ))( fd, v );
}

static int g_close( int fd )
{
    struct eventfd *e = eventfd_get( fd );
    if (e) { e->used = 0; pthread_mutex_destroy( &e->lock ); }
    return ((int (*)( int ))klepton( "close" ))( fd );
}

/* ---- rlimit: Linux resource numbers ----------------------------------------- */
static int rlimit_resource( int r )
{
    switch (r)
    {
    case 0: return RLIMIT_CPU; case 1: return RLIMIT_FSIZE; case 2: return RLIMIT_DATA;
    case 3: return RLIMIT_STACK; case 4: return RLIMIT_CORE; case 5: return RLIMIT_RSS;
    case 6: return RLIMIT_NPROC; case 7: return RLIMIT_NOFILE; case 8: return RLIMIT_MEMLOCK;
    case 9: return RLIMIT_AS; default: return -1;
    }
}
static int g_getrlimit( int r, struct rlimit *l )
{
    int d = rlimit_resource( r );
    if (d < 0) { l->rlim_cur = l->rlim_max = RLIM_INFINITY; return 0; }
    return getrlimit( d, l );
}
static int g_setrlimit( int r, const struct rlimit *l )
{
    int d = rlimit_resource( r );
    if (d < 0) return 0;
    struct rlimit c = *l;
    if (d == RLIMIT_NOFILE && c.rlim_cur > OPEN_MAX) c.rlim_cur = OPEN_MAX;   /* Darwin refuses more */
    return setrlimit( d, &c );
}

/* ---- locale: glibc's locale_t is a struct libstdc++ reads directly ---------- */
struct glibc_locale {
    void *locales[13];               /* [0] holds our Darwin locale_t */
    const unsigned short *ctype_b;   /* offset 0x68 */
    const int32_t *ctype_tolower, *ctype_toupper;
    const char *names[13];
};
extern void *mvr_glibc_ctype_tables( const unsigned short **b, const int32_t **lo, const int32_t **up );

static locale_t darwin_of( const struct glibc_locale *l )
{
    if (l == (void *)-1L) return LC_GLOBAL_LOCALE;
    return l ? (locale_t)l->locales[0] : NULL;
}

static struct glibc_locale *wrap_locale( locale_t d, const char *name )
{
    struct glibc_locale *l = calloc( 1, sizeof *l );
    l->locales[0] = d;
    mvr_glibc_ctype_tables( &l->ctype_b, &l->ctype_tolower, &l->ctype_toupper );
    for (int i = 0; i < 13; i++) l->names[i] = name ? strdup( name ) : "C";
    return l;
}

static const char *darwin_locale_name( const char *name )
{
    if (!name || !*name || !strcmp( name, "C" ) || !strcmp( name, "POSIX" )) return "C";
    if (!strncmp( name, "C.", 2 )) return "en_US.UTF-8";
    return name;
}

static struct glibc_locale *g___newlocale( int mask, const char *name, struct glibc_locale *base )
{
    locale_t d = newlocale( LC_ALL_MASK, darwin_locale_name( name ), NULL );
    if (!d) d = newlocale( LC_ALL_MASK, "C", NULL );
    if (base && base != (void *)-1L) { freelocale( darwin_of( base ) ); free( base ); }
    return wrap_locale( d, name );
}
static struct glibc_locale *g___duplocale( struct glibc_locale *l ) { return wrap_locale( duplocale( darwin_of( l ) ), l && l != (void *)-1L ? l->names[0] : "C" ); }
static void g___freelocale( struct glibc_locale *l ) { if (l && l != (void *)-1L) { freelocale( darwin_of( l ) ); free( l ); } }

static __thread struct glibc_locale *thread_locale;
static struct glibc_locale *g___uselocale( struct glibc_locale *l )
{
    struct glibc_locale *prev = thread_locale ? thread_locale : (void *)-1L;
    if (l) { uselocale( darwin_of( l ) ); thread_locale = l == (void *)-1L ? NULL : l; }
    return prev;
}

/* nl_langinfo items are (category << 16) | index in glibc */
static const char *langinfo( int item )
{
    switch (item)
    {
    case 14: return "UTF-8";                     /* CODESET (_NL_CTYPE_CODESET_NAME) */
    case 0x10000: return ".";                    /* RADIXCHAR */
    case 0x10001: return "";                     /* THOUSEP */
    case 0x20028: return "%a %b %e %H:%M:%S %Y"; /* D_T_FMT */
    case 0x20029: return "%m/%d/%y";             /* D_FMT */
    case 0x2002A: return "%H:%M:%S";             /* T_FMT */
    default: return "";
    }
}
static const char *g_nl_langinfo( int item ) { return langinfo( item ); }
static const char *g___nl_langinfo_l( int item, struct glibc_locale *l ) { return langinfo( item ); }

static double g___strtod_l( const char *s, char **e, struct glibc_locale *l ) { return strtod_l( s, e, darwin_of( l ) ); }
static float g___strtof_l( const char *s, char **e, struct glibc_locale *l ) { return strtof_l( s, e, darwin_of( l ) ); }
static int g___strcoll_l( const char *a, const char *b, struct glibc_locale *l ) { return strcoll_l( a, b, darwin_of( l ) ); }
static size_t g___strxfrm_l( char *d, const char *s, size_t n, struct glibc_locale *l ) { return strxfrm_l( d, s, n, darwin_of( l ) ); }
static int g___wcscoll_l( const wchar_t *a, const wchar_t *b, struct glibc_locale *l ) { return wcscoll_l( a, b, darwin_of( l ) ); }
static size_t g___wcsxfrm_l( wchar_t *d, const wchar_t *s, size_t n, struct glibc_locale *l ) { return wcsxfrm_l( d, s, n, darwin_of( l ) ); }
static size_t g___strftime_l( char *d, size_t n, const char *f, const struct tm *t, struct glibc_locale *l ) { return strftime_l( d, n, f, t, darwin_of( l ) ); }
static size_t g___wcsftime_l( wchar_t *d, size_t n, const wchar_t *f, const struct tm *t, struct glibc_locale *l ) { return wcsftime_l( d, n, f, t, darwin_of( l ) ); }
static wint_t g___towlower_l( wint_t c, struct glibc_locale *l ) { return towlower_l( c, darwin_of( l ) ); }
static wint_t g___towupper_l( wint_t c, struct glibc_locale *l ) { return towupper_l( c, darwin_of( l ) ); }
static wctype_t g___wctype_l( const char *n, struct glibc_locale *l ) { return wctype_l( n, darwin_of( l ) ); }
static int g___iswctype_l( wint_t c, wctype_t t, struct glibc_locale *l ) { return iswctype_l( c, t, darwin_of( l ) ); }
static long long g___strtoll_l( const char *s, char **e, int b, struct glibc_locale *l ) { return strtoll_l( s, e, b, darwin_of( l ) ); }
static unsigned long long g___strtoull_l( const char *s, char **e, int b, struct glibc_locale *l ) { return strtoull_l( s, e, b, darwin_of( l ) ); }

/* ---- fortify (_chk) variants: drop the extra checks, keep the call ---------- */
typedef int (*vfprintf_fn)( FILE *, const char *, kl_va * );
typedef int (*vasprintf_fn)( char **, const char *, kl_va * );
int mvrh_printf_chk( int flag, const char *fmt, kl_va *va ) { return ((vfprintf_fn)klepton( "vfprintf" ))( stdout, fmt, va ); }
int mvrh_fprintf_chk( FILE *f, int flag, const char *fmt, kl_va *va ) { return ((vfprintf_fn)klepton( "vfprintf" ))( f, fmt, va ); }
int mvrh_asprintf_chk( char **s, int flag, const char *fmt, kl_va *va ) { return ((vasprintf_fn)klepton( "vasprintf" ))( s, fmt, va ); }
int mvrh_syslog_chk( int pri, int flag, const char *fmt, kl_va *va ) { return 0; }
static int g___vfprintf_chk( FILE *f, int flag, const char *fmt, kl_va *va ) { return ((vfprintf_fn)klepton( "vfprintf" ))( f, fmt, va ); }
static int g___vasprintf_chk( char **s, int flag, const char *fmt, kl_va *va ) { return ((vasprintf_fn)klepton( "vasprintf" ))( s, fmt, va ); }
static size_t g___fread_chk( void *p, size_t plen, size_t sz, size_t n, FILE *f )
{
    return ((size_t (*)(void *, size_t, size_t, FILE *))klepton( "fread" ))( p, sz, n, f );
}
static char *g___realpath_chk( const char *p, char *r, size_t len ) { return ((char *(*)(const char *, char *))klepton( "realpath" ))( p, r ); }
static size_t g___mbstowcs_chk( wchar_t *d, const char *s, size_t n, size_t dlen ) { return mbstowcs( d, s, n ); }
static void g___explicit_bzero_chk( void *p, size_t n, size_t dlen ) { memset_s( p, n, 0, n ); }
static long g___fdelt_chk( long fd ) { return fd / 64; }

/* errx/warn and friends: variadic, go through the AAPCS64 va thunks */
void mvrh_warn( const char *fmt, kl_va *va )
{
    fprintf( stderr, "%s: ", getprogname() );
    if (fmt) ((vfprintf_fn)klepton( "vfprintf" ))( stderr, fmt, va );
    fputc( '\n', stderr );
}
void mvrh_errx( int code, const char *fmt, kl_va *va ) { mvrh_warn( fmt, va ); exit( code ); }

/* ---- small things ----------------------------------------------------------- */
static void g___assert_fail( const char *a, const char *file, unsigned line, const char *fn )
{
    fprintf( stderr, "%s:%u: %s: assertion `%s' failed\n", file, line, fn ? fn : "?", a );
    abort();
}
static size_t g___fpending( FILE *f ) { return 0; }
static int g___fsetlocking( FILE *f, int type ) { return 1; }
static int g___uflow( FILE *f ) { return fgetc( f ); }
static char *g_fgets_unlocked( char *s, int n, FILE *f ) { return ((char *(*)(char *, int, FILE *))klepton( "fgets" ))( s, n, f ); }
static int g_fileno_unlocked( FILE *f ) { return ((int (*)(FILE *))klepton( "fileno" ))( f ); }
static ssize_t g___getdelim( char **l, size_t *n, int d, FILE *f ) { return getdelim( l, n, d, f ); }
static char *g___xpg_basename( char *p ) { return basename( p ); }
static int g___xpg_strerror_r( int e, char *b, size_t n ) { return strerror_r( mvr_errno_to_darwin( e ), b, n ) ? 34 : 0; }
static char *g_secure_getenv( const char *n ) { return getenv( n ); }
static char *g_ctermid( char *s ) { if (s) { strcpy( s, "/dev/tty" ); return s; } return "/dev/tty"; }
static int g_getdtablesize(void) { return getdtablesize(); }
static void g_malloc_stats(void) { }
static void *g_reallocarray( void *p, size_t n, size_t sz )
{
    if (sz && n > SIZE_MAX / sz) { errno = ENOMEM; return NULL; }
    return realloc( p, n * sz );
}
static char *g_gettext( const char *m ) { return (char *)m; }
static char *g_dgettext( const char *d, const char *m ) { return (char *)m; }
static char *g_dcgettext( const char *d, const char *m, int c ) { return (char *)m; }
static char *g_dcngettext( const char *d, const char *m, const char *p, unsigned long n, int c ) { return (char *)(n == 1 ? m : p); }
static char *g_textdomain( const char *d ) { return (char *)(d ? d : "messages"); }
static char *g_bindtextdomain( const char *d, const char *dir ) { return (char *)dir; }
static char *g_bind_textdomain_codeset( const char *d, const char *c ) { return (char *)c; }
static __thread int h_errno_linux;
static int *g___h_errno_location(void) { return &h_errno_linux; }
static int g_sched_getcpu(void) { uint64_t c; __asm__ volatile( "mrs %0, tpidr_el0" : "=r"(c) ); return (int)(c & 0xff); }
static int g_pthread_getaffinity_np( pthread_t t, size_t size, void *set )
{
    memset( set, 0, size );
    long n = sysconf( _SC_NPROCESSORS_ONLN );
    for (long i = 0; i < n && i < (long)size * 8; i++) ((unsigned char *)set)[i / 8] |= 1 << (i % 8);
    return 0;
}
static int g_sched_getscheduler( int pid ) { return 0; }      /* SCHED_OTHER */
static int g_sched_getparam( int pid, int *param ) { *param = 0; return 0; }
static void *g___sched_cpualloc( size_t n ) { return calloc( 1, (n + 63) / 64 * 8 ); }
static int g___sched_cpucount( size_t size, const unsigned long *set )
{
    int c = 0;
    for (size_t i = 0; i < size / sizeof(long); i++) c += __builtin_popcountl( set[i] );
    return c;
}
static int g_random_r( void *buf, int32_t *result ) { *result = (int32_t)(random() & 0x7fffffff); return 0; }
static int g_initstate_r( unsigned seed, char *state, size_t n, void *buf ) { srandom( seed ); return 0; }

/* glibc qsort_r(base, n, size, cmp(a, b, arg), arg); BSD swaps the context */
struct qsort_ctx { int (*cmp)( const void *, const void *, void * ); void *arg; };
static int qsort_trampoline( void *c, const void *a, const void *b ) { struct qsort_ctx *x = c; return x->cmp( a, b, x->arg ); }
static void g_qsort_r( void *base, size_t n, size_t sz, int (*cmp)( const void *, const void *, void * ), void *arg )
{
    struct qsort_ctx c = { cmp, arg };
    qsort_r( base, n, sz, &c, qsort_trampoline );
}

/* glibc's strverscmp: compare with numeric runs ordered as numbers */
static int g_strverscmp( const char *a, const char *b )
{
    while (*a && *a == *b) { a++; b++; }
    if (isdigit( (unsigned char)*a ) && isdigit( (unsigned char)*b ))
    {
        size_t la = strspn( a, "0123456789" ), lb = strspn( b, "0123456789" );
        if (la != lb) return la < lb ? -1 : 1;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

/* versionsort over glibc's struct dirent64 (d_name at offset 19) */
static int g_versionsort( const void **a, const void **b )
{
    return g_strverscmp( (const char *)*a + 19, (const char *)*b + 19 );
}

/* glibc-only data */
static int libc_single_threaded;
static char *progname_full = "hlvr", *progname_short = "hlvr";
static long timezone_linux;
static int daylight_linux;
static void *libc_stack_end;
static const unsigned char in6addr_loopback_linux[16] = { [15] = 1 };

/* Memory below 4 GB. Darwin keeps it as the executable's PAGEZERO, so allocators that
 * insist on low addresses (LuaJIT's arena probes from 0x10000 up) get nothing. With the
 * cross-architecture entitlement PAGEZERO can be mapped over, as Wine's loader does: we
 * reserve 64 KB - 4 GB at startup and hand out ranges from it when a hint asks for them. */
#define LINUX_MAP_FIXED 0x10
#define LINUX_MAP_ANONYMOUS 0x20
#define LOW_START 0x10000ull
#define LOW_END 0x100000000ull
#define LOW_GRAN 0x10000ull
#define LOW_SLOTS ((LOW_END - LOW_START) / LOW_GRAN)
static unsigned char *low_used;   /* one byte per 64 KB granule */
static pthread_mutex_t low_lock = PTHREAD_MUTEX_INITIALIZER;

void mvr_low_memory_init(void)
{
    void *p = mmap( (void *)LOW_START, LOW_END - LOW_START, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED | MAP_NORESERVE, -1, 0 );
    if (p == MAP_FAILED) return;   /* no entitlement: low hints just fall through */
    low_used = calloc( LOW_SLOTS, 1 );
}

static int in_low( uintptr_t a, size_t len ) { return low_used && a >= LOW_START && a + len <= LOW_END; }

static void *g_mmap( void *addr, size_t len, int prot, int flags, int fd, off_t off )
{
    uintptr_t a = (uintptr_t)addr & ~(LOW_GRAN - 1);
    size_t size = (len + ((uintptr_t)addr - a) + LOW_GRAN - 1) & ~(LOW_GRAN - 1);
    if (addr && !(flags & LINUX_MAP_FIXED) && (flags & LINUX_MAP_ANONYMOUS) && fd == -1 && in_low( a, size ))
    {
        pthread_mutex_lock( &low_lock );
        size_t first = (a - LOW_START) / LOW_GRAN, n = size / LOW_GRAN, i;
        for (i = 0; i < n && !low_used[first + i]; i++) {}
        void *p = MAP_FAILED;
        if (i == n)
        {
            p = mmap( (void *)a, size, prot & 7, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0 );
            if (p != MAP_FAILED) memset( low_used + first, 1, n );
        }
        pthread_mutex_unlock( &low_lock );
        if (p != MAP_FAILED) return (void *)(a + ((uintptr_t)addr - a));
    }
    return ((void *(*)( void *, size_t, int, int, int, off_t ))klepton( "mmap" ))( addr, len, prot, flags, fd, off );
}

int mvr_jit_mprotect( void *addr, size_t len, int prot );
void mvr_jit_unmap( void *addr, size_t len );

static int g_mprotect( void *addr, size_t len, int prot )
{
    if (!mvr_jit_mprotect( addr, len, prot )) return 0;
    return ((int (*)( void *, size_t, int ))klepton( "mprotect" ))( addr, len, prot );
}

static int g_munmap( void *addr, size_t len )
{
    mvr_jit_unmap( addr, len );
    uintptr_t a = (uintptr_t)addr;
    if (in_low( a, len ) && !(a & (LOW_GRAN - 1)))
    {
        size_t size = (len + LOW_GRAN - 1) & ~(LOW_GRAN - 1);
        pthread_mutex_lock( &low_lock );
        mmap( addr, size, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED | MAP_NORESERVE, -1, 0 );
        memset( low_used + (a - LOW_START) / LOW_GRAN, 0, size / LOW_GRAN );
        pthread_mutex_unlock( &low_lock );
        return 0;
    }
    return ((int (*)( void *, size_t ))klepton( "munmap" ))( addr, len );
}

/* dlmopen(namespace, file, flags): we have one namespace */
static void *g_dlmopen( long lmid, const char *file, int flags )
{
    return ((void *(*)( const char *, int ))klepton( "dlopen" ))( file, flags );
}

/* ---- the table -------------------------------------------------------------- */
extern void mvrv_printf_chk(void), mvrv_fprintf_chk(void), mvrv_asprintf_chk(void), mvrv_syslog_chk(void),
            mvrv_warn(void), mvrv_errx(void);

static const struct { const char *name; void *fn; } table[] = {
    { "fcntl", g_fcntl }, { "fcntl64", g_fcntl },
    { "eventfd", g_eventfd }, { "eventfd_write", g_eventfd_write }, { "eventfd_read", g_eventfd_read },
    { "write", g_write }, { "read", g_read }, { "close", g_close },

    { "semget", g_semget }, { "semctl", g_semctl }, { "semop", g_semop }, { "semtimedop", g_semtimedop },
    { "dlmopen", g_dlmopen }, { "mmap", g_mmap }, { "mmap64", g_mmap }, { "munmap", g_munmap }, { "mprotect", g_mprotect },
    { "readlink", g_readlink }, { "readlinkat", g_readlinkat },
    { "sysconf", g_sysconf }, { "__sysconf", g_sysconf }, { "get_nprocs", g_get_nprocs },
    { "get_nprocs_conf", g_get_nprocs },
    { "getauxval", g___getauxval }, { "__getauxval", g___getauxval },
    { "__xstat", g___xstat }, { "__xstat64", g___xstat }, { "__lxstat", g___lxstat }, { "__lxstat64", g___lxstat },
    { "__fxstat", g___fxstat }, { "__fxstat64", g___fxstat }, { "__xmknod", g___xmknod },
    { "fstatfs", g_fstatfs }, { "fstatfs64", g_fstatfs }, { "statfs", g_statfs }, { "statfs64", g_statfs },
    { "statvfs", g_statvfs }, { "statvfs64", g_statvfs }, { "fstatvfs", g_fstatvfs }, { "fstatvfs64", g_fstatvfs },
    { "faccessat", g_faccessat }, { "eaccess", g_eaccess }, { "euidaccess", g_eaccess },
    { "getrlimit", g_getrlimit }, { "getrlimit64", g_getrlimit }, { "setrlimit", g_setrlimit }, { "setrlimit64", g_setrlimit },
    { "__newlocale", g___newlocale }, { "newlocale", g___newlocale }, { "__duplocale", g___duplocale },
    { "duplocale", g___duplocale }, { "__freelocale", g___freelocale }, { "freelocale", g___freelocale },
    { "__uselocale", g___uselocale }, { "uselocale", g___uselocale },
    { "nl_langinfo", g_nl_langinfo }, { "__nl_langinfo_l", g___nl_langinfo_l }, { "nl_langinfo_l", g___nl_langinfo_l },
    { "__strtod_l", g___strtod_l }, { "strtod_l", g___strtod_l }, { "__strtof_l", g___strtof_l },
    { "__strcoll_l", g___strcoll_l }, { "__strxfrm_l", g___strxfrm_l }, { "__wcscoll_l", g___wcscoll_l },
    { "__wcsxfrm_l", g___wcsxfrm_l }, { "__strftime_l", g___strftime_l }, { "__wcsftime_l", g___wcsftime_l },
    { "__towlower_l", g___towlower_l }, { "__towupper_l", g___towupper_l }, { "__wctype_l", g___wctype_l },
    { "__iswctype_l", g___iswctype_l }, { "__isoc23_strtoll_l", g___strtoll_l }, { "__isoc23_strtoull_l", g___strtoull_l },
    { "__strtoll_l", g___strtoll_l }, { "__strtoull_l", g___strtoull_l },
    { "__printf_chk", mvrv_printf_chk }, { "__fprintf_chk", mvrv_fprintf_chk },
    { "__asprintf_chk", mvrv_asprintf_chk }, { "__syslog_chk", mvrv_syslog_chk },
    { "__vfprintf_chk", g___vfprintf_chk }, { "__vasprintf_chk", g___vasprintf_chk },
    { "__fread_chk", g___fread_chk }, { "__realpath_chk", g___realpath_chk }, { "__mbstowcs_chk", g___mbstowcs_chk },
    { "__explicit_bzero_chk", g___explicit_bzero_chk }, { "__fdelt_chk", g___fdelt_chk },
    { "warn", mvrv_warn }, { "warnx", mvrv_warn }, { "err", mvrv_errx }, { "errx", mvrv_errx },
    { "__assert_fail", g___assert_fail }, { "__fpending", g___fpending }, { "__fsetlocking", g___fsetlocking },
    { "__uflow", g___uflow }, { "fgets_unlocked", g_fgets_unlocked }, { "fileno_unlocked", g_fileno_unlocked },
    { "__getdelim", g___getdelim }, { "__xpg_basename", g___xpg_basename }, { "__xpg_strerror_r", g___xpg_strerror_r },
    { "secure_getenv", g_secure_getenv }, { "__secure_getenv", g_secure_getenv }, { "ctermid", g_ctermid },
    { "getdtablesize", g_getdtablesize }, { "malloc_stats", g_malloc_stats }, { "reallocarray", g_reallocarray },
    { "gettext", g_gettext }, { "dgettext", g_dgettext }, { "dcgettext", g_dcgettext }, { "dcngettext", g_dcngettext },
    { "textdomain", g_textdomain }, { "bindtextdomain", g_bindtextdomain },
    { "bind_textdomain_codeset", g_bind_textdomain_codeset }, { "__h_errno_location", g___h_errno_location },
    { "sched_getcpu", g_sched_getcpu }, { "pthread_getaffinity_np", g_pthread_getaffinity_np },
    { "pthread_setaffinity_np", zero }, { "sched_setaffinity", zero }, { "sched_getscheduler", g_sched_getscheduler },
    { "sched_getparam", g_sched_getparam }, { "sched_setscheduler", zero }, { "sched_setparam", zero },
    { "__sched_cpualloc", g___sched_cpualloc }, { "__sched_cpufree", free }, { "__sched_cpucount", g___sched_cpucount },
    { "random_r", g_random_r }, { "initstate_r", g_initstate_r }, { "srandom_r", zero },
    { "qsort_r", g_qsort_r }, { "strverscmp", g_strverscmp }, { "versionsort", g_versionsort },
    { "versionsort64", g_versionsort }, { "execlp", enosys }, { "execl", enosys }, { "execle", enosys },
    { "__libc_single_threaded", &libc_single_threaded }, { "__progname_full", &progname_full },
    { "program_invocation_name", &progname_full }, { "program_invocation_short_name", &progname_short },
    { "__progname", &progname_short }, { "timezone", &timezone_linux }, { "__timezone", &timezone_linux },
    { "daylight", &daylight_linux }, { "__daylight", &daylight_linux }, { "__libc_stack_end", &libc_stack_end },
    { "in6addr_loopback", (void *)in6addr_loopback_linux },
    { "_Exit", _exit }, { "malloc_trim", zero }, { "posix_fadvise", zero }, { "posix_fadvise64", zero },
    { "posix_fallocate", zero }, { "posix_fallocate64", zero }, { "fallocate", enosys }, { "fallocate64", enosys },
    { "readahead", zero }, { "syncfs", zero },
    /* Linux-only kernel features: callers fall back when these say ENOSYS */
    { "statx", enosys }, { "close_range", enosys }, { "copy_file_range", enosys }, { "splice", enosys },
    { "mremap", enosys_null }, { "sendfile", enosys }, { "sendfile64", enosys }, { "recvmmsg", enosys },
    { "sendmmsg", enosys }, { "mount", enosys }, { "umount", enosys }, { "umount2", enosys }, { "unshare", enosys },
    { "setns", enosys }, { "fsopen", enosys }, { "fsconfig", enosys }, { "fsmount", enosys }, { "fspick", enosys },
    { "move_mount", enosys }, { "open_tree", enosys }, { "mount_setattr", enosys }, { "sigtimedwait", enosys },
    { "getcontext", enosys }, { "setcontext", enosys }, { "makecontext", zero }, { "swapcontext", enosys },
    { "fts64_open", enosys_null }, { "fts64_read", enosys_null }, { "fts64_close", zero }, { "fts64_set", zero },
    { "nftw64", enosys }, { "nftw", enosys }, { "scandirat", enosys }, { "setmntent", enosys_null },
    { "getmntent_r", enosys_null }, { "endmntent", zero }, { "hasmntopt", enosys_null },
    { "getttynam", enosys_null }, { "endservent", zero }, { "getservbyname_r", enosys },
    { "res_nquery", enosys }, { "__res_ninit", enosys }, { "__res_nclose", zero }, { "dn_expand", enosys },
    { "getxattr", enosys }, { "lgetxattr", enosys }, { "fgetxattr", enosys }, { "setxattr", enosys },
    { "lsetxattr", enosys }, { "fsetxattr", enosys }, { "listxattr", enosys }, { "llistxattr", enosys },
    { "flistxattr", enosys }, { "removexattr", enosys }, { "execvpe", enosys }, { "_dl_find_object", enosys },
    { "__isoc23_swscanf", enosys }, { "__isoc23_vswscanf", enosys }, { "__isoc23_wcstoll", wcstoll },
    { "__isoc23_wcstoull", wcstoull }, { "shmctl", zero },
};

/* glibc spellings of functions Klepton (or we) already answer */
static const struct { const char *glibc, *as; } aliases[] = {
    { "__isoc23_sscanf", "sscanf" }, { "__isoc99_sscanf", "sscanf" }, { "__isoc23_fscanf", "fscanf" },
    { "__isoc99_fscanf", "fscanf" }, { "__isoc23_vsscanf", "vsscanf" }, { "__isoc99_vsscanf", "vsscanf" },
    { "__isoc23_vfscanf", "vfscanf" }, { "__isoc23_strtol", "strtol" }, { "__isoc23_strtoll", "strtoll" },
    { "__isoc23_strtoul", "strtoul" }, { "__isoc23_strtoull", "strtoull" }, { "__isoc23_strtoimax", "strtoimax" },
    { "__isoc23_strtoumax", "strtoumax" }, { "__open64_2", "open" }, { "__openat_2", "openat" },
    { "__longjmp_chk", "siglongjmp" }, { "creat64", "creat" },
};

/* Darwin libc functions whose glibc ABI is identical (scalar/pointer arguments,
 * matching or opaque structs, not variadic) */
static const char *const darwin_same[] = {
    "floor", "lround", "lroundf", "nextafterf", "cabsf", "cexpf", "fegetround", "fesetround", "memmem",
    "strndup", "bcmp", "asctime", "asctime_r", "backtrace", "backtrace_symbols", "catopen", "catgets",
    "catclose", "futimes", "getloadavg", "lchmod", "putenv", "tmpnam", "ttyname", "wcwidth", "wcswidth",
    "wcscasecmp", "wcsncasecmp", "wcsdup", "wcslcat", "wcslcpy", "wcsstr", "wctomb", "iconv", "iconv_open",
    "iconv_close", "pthread_cancel", "pthread_setcanceltype", "pthread_getname_np",
    "pthread_attr_setinheritsched", "posix_spawn", "posix_spawnp", "posix_spawnattr_init",
    "posix_spawnattr_destroy", "posix_spawnattr_setflags", "posix_spawnattr_setsigdefault",
    "posix_spawn_file_actions_init", "posix_spawn_file_actions_destroy", "posix_spawn_file_actions_addclose",
    "posix_spawn_file_actions_adddup2", "shmget", "shmat", "shmdt", "mkostemp", "mkostemps", "execvp",
    "_setjmp", "_longjmp", "__sigsetjmp", "dirfd", "posix_madvise", "__mbrlen",
    "wcstoll", "wcstoull", "strtoimax", "strtoumax", "vfscanf",
};

/* only libSystem: RTLD_DEFAULT would also find Steam's macOS tier0/vstdlib exports */
static void *libsystem( void )
{
    static void *h;
    if (!h) h = dlopen( "/usr/lib/libSystem.B.dylib", RTLD_NOLOAD | RTLD_LAZY );
    return h;
}

void *mvr_glibc_ext_lookup( const char *name )
{
    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++)
        if (!strcmp( table[i].name, name )) return table[i].fn;
    for (size_t i = 0; i < sizeof aliases / sizeof aliases[0]; i++)
        if (!strcmp( aliases[i].glibc, name ))
        {
            void *p = mvr_glibc_lookup( aliases[i].as );
            if (!p) p = klepton( aliases[i].as );
            if (!p) p = dlsym( libsystem(), aliases[i].as );
            return p;
        }
    for (size_t i = 0; i < sizeof darwin_same / sizeof darwin_same[0]; i++)
        if (!strcmp( darwin_same[i], name ))
        {
            if (!strcmp( name, "__sigsetjmp" )) return dlsym( libsystem(), "sigsetjmp" );
            if (!strcmp( name, "__mbrlen" )) return dlsym( libsystem(), "mbrlen" );
            return dlsym( libsystem(), name );
        }
    /* foo64 is foo on a 64-bit ABI: take whatever answers foo */
    size_t n = strlen( name );
    if (n > 2 && !strcmp( name + n - 2, "64" ) && n < 64)
    {
        char base[64];
        memcpy( base, name, n - 2 ); base[n - 2] = 0;
        void *p = mvr_glibc_lookup( base );
        if (!p) p = klepton( base );
        if (!p) p = dlsym( libsystem(), base );
        if (p) return p;
    }
    return NULL;
}
