#include "glibc.h"
#include "../tls/mvr_tls.h"
#include "../deps.h"
#include "klepton.h"
#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ---- errno ----------------------------------------------------------------
 * Linux and Darwin agree on 1-34 except 11 (Linux EAGAIN, Darwin EDEADLK); above
 * that they diverge. The guest sees a per-thread Linux errno: refreshed from
 * Darwin's every time it asks for the address, and pushed back to Darwin when the
 * guest has written it in between (errno = 0 before strtol, say). */
static const struct { short linux_e, darwin_e; } errno_map[] = {
    { 11, EAGAIN }, { 35, EDEADLK }, { 36, ENAMETOOLONG }, { 37, ENOLCK }, { 38, ENOSYS },
    { 39, ENOTEMPTY }, { 40, ELOOP }, { 42, ENOMSG }, { 43, EIDRM }, { 61, ENODATA },
    { 62, ETIME }, { 71, EPROTO }, { 75, EOVERFLOW }, { 84, EILSEQ }, { 88, ENOTSOCK },
    { 89, EDESTADDRREQ }, { 90, EMSGSIZE }, { 91, EPROTOTYPE }, { 92, ENOPROTOOPT },
    { 93, EPROTONOSUPPORT }, { 95, EOPNOTSUPP }, { 97, EAFNOSUPPORT }, { 98, EADDRINUSE },
    { 99, EADDRNOTAVAIL }, { 100, ENETDOWN }, { 101, ENETUNREACH }, { 103, ECONNABORTED },
    { 104, ECONNRESET }, { 105, ENOBUFS }, { 106, EISCONN }, { 107, ENOTCONN },
    { 110, ETIMEDOUT }, { 111, ECONNREFUSED }, { 113, EHOSTUNREACH }, { 114, EALREADY },
    { 115, EINPROGRESS }, { 125, ECANCELED },
};

int mvr_errno_to_linux( int e )
{
    if (e == EDEADLK) return 35;
    for (size_t i = 0; i < sizeof errno_map / sizeof errno_map[0]; i++)
        if (errno_map[i].darwin_e == e) return errno_map[i].linux_e;
    return e;
}

int mvr_errno_to_darwin( int e )
{
    for (size_t i = 0; i < sizeof errno_map / sizeof errno_map[0]; i++)
        if (errno_map[i].linux_e == e) return errno_map[i].darwin_e;
    return e;
}

static __thread int linux_errno, linux_errno_given;

static int *g___errno_location(void)
{
    if (linux_errno != linux_errno_given) errno = mvr_errno_to_darwin( linux_errno );
    linux_errno = linux_errno_given = mvr_errno_to_linux( errno );
    return &linux_errno;
}

/* ---- ctype: glibc's table bits (bits/ctype.h, little endian) --------------- */
enum { G_UPPER = 0x100, G_LOWER = 0x200, G_ALPHA = 0x400, G_DIGIT = 0x800, G_XDIGIT = 0x1000,
       G_SPACE = 0x2000, G_PRINT = 0x4000, G_GRAPH = 0x8000, G_BLANK = 0x1, G_CNTRL = 0x2,
       G_PUNCT = 0x4, G_ALNUM = 0x8 };
static unsigned short ctype_b[384];
static int32_t ctype_lower[384], ctype_upper[384];
static const unsigned short *ctype_b_ptr = ctype_b + 128;
static const int32_t *ctype_lower_ptr = ctype_lower + 128, *ctype_upper_ptr = ctype_upper + 128;

static void init_ctype(void)
{
    for (int c = -128; c < 256; c++)
    {
        unsigned short v = 0;
        int u = (unsigned char)c;   /* C locale: negative indexes mirror 128-255, all zero */
        if (c >= 0 && c < 128)
        {
            if (isupper( u )) v |= G_UPPER;
            if (islower( u )) v |= G_LOWER;
            if (isalpha( u )) v |= G_ALPHA;
            if (isdigit( u )) v |= G_DIGIT;
            if (isxdigit( u )) v |= G_XDIGIT;
            if (isspace( u )) v |= G_SPACE;
            if (isprint( u )) v |= G_PRINT;
            if (isgraph( u )) v |= G_GRAPH;
            if (isblank( u )) v |= G_BLANK;
            if (iscntrl( u )) v |= G_CNTRL;
            if (ispunct( u )) v |= G_PUNCT;
            if (isalnum( u )) v |= G_ALNUM;
        }
        ctype_b[c + 128] = v;
        ctype_lower[c + 128] = c >= 0 && c < 128 ? tolower( c ) : c;
        ctype_upper[c + 128] = c >= 0 && c < 128 ? toupper( c ) : c;
    }
}

static const unsigned short **g___ctype_b_loc(void) { return &ctype_b_ptr; }
static const int32_t **g___ctype_tolower_loc(void) { return &ctype_lower_ptr; }
static const int32_t **g___ctype_toupper_loc(void) { return &ctype_upper_ptr; }

/* ---- stat: glibc aarch64 struct stat (asm-generic layout, 128 bytes) -------- */
struct linux_stat {
    uint64_t st_dev, st_ino;
    uint32_t st_mode, st_nlink, st_uid, st_gid;
    uint64_t st_rdev, __pad1;
    int64_t  st_size;
    int32_t  st_blksize, __pad2;
    int64_t  st_blocks;
    int64_t  st_atime_sec, st_atime_nsec, st_mtime_sec, st_mtime_nsec, st_ctime_sec, st_ctime_nsec;
    uint32_t reserved[2];
};
_Static_assert( sizeof(struct linux_stat) == 128, "glibc aarch64 struct stat is 128 bytes" );

static void to_linux_stat( const struct stat *s, struct linux_stat *l )
{
    memset( l, 0, sizeof *l );
    l->st_dev = (uint64_t)s->st_dev; l->st_ino = s->st_ino;
    l->st_mode = s->st_mode; l->st_nlink = s->st_nlink; l->st_uid = s->st_uid; l->st_gid = s->st_gid;
    l->st_rdev = (uint64_t)s->st_rdev; l->st_size = s->st_size;
    l->st_blksize = s->st_blksize; l->st_blocks = s->st_blocks;
    l->st_atime_sec = s->st_atimespec.tv_sec; l->st_atime_nsec = s->st_atimespec.tv_nsec;
    l->st_mtime_sec = s->st_mtimespec.tv_sec; l->st_mtime_nsec = s->st_mtimespec.tv_nsec;
    l->st_ctime_sec = s->st_ctimespec.tv_sec; l->st_ctime_nsec = s->st_ctimespec.tv_nsec;
}

extern const char *mvr_glibc_exe_path(void);
static int is_proc_exe( const char *p ) { return p && (!strcmp( p, "/proc/self/exe" ) || !strcmp( p, "/proc/thread-self/exe" )); }

static int g_stat( const char *path, struct linux_stat *l )
{
    if (is_proc_exe( path )) path = mvr_glibc_exe_path();
    struct stat s; int r = stat( path, &s ); if (!r) to_linux_stat( &s, l ); return r;
}
static int g_lstat( const char *path, struct linux_stat *l )
{
    struct stat s; int r;
    if (is_proc_exe( path ))
    {
        /* Linux shows a symlink whose size is the length of its target */
        if ((r = stat( mvr_glibc_exe_path(), &s ))) return r;
        to_linux_stat( &s, l );
        l->st_mode = 0120777;
        l->st_size = strlen( mvr_glibc_exe_path() );
        return 0;
    }
    r = lstat( path, &s ); if (!r) to_linux_stat( &s, l ); return r;
}
static int g_fstat( int fd, struct linux_stat *l )
{
    struct stat s; int r = fstat( fd, &s ); if (!r) to_linux_stat( &s, l ); return r;
}

/* ---- pthread: every guest thread gets Klepton's and our TLS set up first --- */
typedef struct { void *(*fn)(void *); void *arg; } thread_start;

static void *thread_trampoline( void *p )
{
    thread_start s = *(thread_start *)p;
    free( p );
    kl_thread_init();
    mvr_tls_thread_init();
    return s.fn( s.arg );
}

static int g_pthread_create( pthread_t *t, const void *attr, void *(*fn)(void *), void *arg )
{
    thread_start *s = malloc( sizeof *s );
    if (!s) return 12;
    *s = (thread_start){ fn, arg };
    /* glibc's pthread_attr_t is 64 bytes and unrelated to Darwin's; defaults for now */
    int r = pthread_create( t, NULL, thread_trampoline, s );
    if (r) free( s );
    return mvr_errno_to_linux( r );
}

static int g_pthread_join( pthread_t t, void **ret ) { return mvr_errno_to_linux( pthread_join( t, ret ) ); }

void *mvr_glibc_ctype_tables( const unsigned short **b, const int32_t **lo, const int32_t **up )
{
    *b = ctype_b_ptr; *lo = ctype_lower_ptr; *up = ctype_upper_ptr;
    return NULL;
}

/* ---- process start: _start hands us main; init_array already ran at load ---- */
extern char **environ;

static int g___libc_start_main( int (*main_fn)(int, char **, char **), int argc, char **argv,
                                void (*init)(void), void (*fini)(void), void (*rtld_fini)(void),
                                void *stack_end )
{
    exit( main_fn( argc, argv, environ ) );
}

/* ---- the table ------------------------------------------------------------- */
static const struct { const char *name; void *fn; } table[] = {
    { "__errno_location", g___errno_location },
    { "__ctype_b_loc", g___ctype_b_loc },
    { "__ctype_tolower_loc", g___ctype_tolower_loc },
    { "__ctype_toupper_loc", g___ctype_toupper_loc },
    { "stat", g_stat }, { "stat64", g_stat }, { "lstat", g_lstat }, { "lstat64", g_lstat },
    { "fstat", g_fstat }, { "fstat64", g_fstat },
    { "pthread_create", g_pthread_create },
    { "pthread_join", g_pthread_join },
    { "__cxa_thread_atexit_impl", mvr_tls_thread_atexit },
    { "__libc_start_main", g___libc_start_main },
};

/* a host built on this one (Inversa) answers before everything else */
void *(*mvr_glibc_extra)( const char *name );

void *mvr_glibc_lookup( const char *name )
{
    if (mvr_glibc_extra)
    {
        void *p = mvr_glibc_extra( name );
        if (p) return p;
    }
    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++)
        if (!strcmp( table[i].name, name )) return table[i].fn;
    void *p = mvr_glibc_ext_lookup( name );
    return p ? p : mvr_hostlib_lookup( name );
}

void mvr_low_memory_init(void);

void mvr_glibc_init(void)
{
    init_ctype();
    mvr_low_memory_init();
    kl_shim_override = mvr_glibc_lookup;
}
