// Containers (inversa bash): a guest whose / is its own root directory, WSL-style. FEX already
// looks inside FEX_ROOTFS first, but falls back to the host path when a file isn't there, so
// /Users and the rest of the Mac show through. Here every absolute path the guest (or FEX on
// its behalf) hands the host is kept inside the root, except /mnt/mac, which is the Mac home
// folder, and the few host paths FEX and the kernel need themselves.
#include "kernel.h"
#include "syscall_names.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void *mvr_glibc_lookup( const char *name );

int inv_container;
static char root[PATH_MAX], mac[PATH_MAX], home[PATH_MAX];
static size_t rootlen, maclen;

static int under( const char *p, const char *dir, size_t n )
{
    return n && !strncmp( p, dir, n ) && (p[n] == 0 || p[n] == '/');
}

void inv_container_init( const char *inversa_home )
{
    const char *r = getenv( "INVERSA_CONTAINER" );
    if (!r || !*r || !realpath( r, root )) return;
    rootlen = strlen( root );
    const char *m = getenv( "INVERSA_CONTAINER_MAC" );
    if (m && realpath( m, mac )) maclen = strlen( mac );
    if (!inversa_home || !realpath( inversa_home, home )) home[0] = 0;
    inv_container = 1;
}

// a path the guest gave -> the host path it means
const char *inv_container_path( const char *p, char *buf )
{
    if (!inv_container || !p || p[0] != '/') return p;
    if (under( p, root, rootlen )) return p;
    if (under( p, "/mnt/mac", 8 ))
    {
        if (!maclen) { snprintf( buf, PATH_MAX, "%s/mnt/mac%s", root, p + 8 ); return buf; }
        snprintf( buf, PATH_MAX, "%s%s", mac, p + 8 );
        return buf;
    }
    if (under( p, "/dev", 4 ) || under( p, "/proc", 5 ) || under( p, home, strlen( home ) )) return p;
    if (!strncmp( p, "/tmp/klepton-proc.", 18 ) || !strncmp( p, "/private/tmp/klepton-proc.", 26 )) return p;   // Klepton's /proc and /sys
    snprintf( buf, PATH_MAX, "%s%s", root, strcmp( p, "/" ) ? p : "" );
    return buf;
}

// a host path going back to the guest (getcwd), rewritten in place
static void to_guest( char *p, size_t cap )
{
    char tmp[PATH_MAX];
    if (under( p, root, rootlen )) snprintf( tmp, sizeof tmp, "%s", p[rootlen] ? p + rootlen : "/" );
    else if (under( p, mac, maclen )) snprintf( tmp, sizeof tmp, "/mnt/mac%s", p + maclen );
    else return;
    snprintf( p, cap, "%s", tmp );
}

// cd .. from the root, or into a host directory, lands back at the root
static void stay_inside(void)
{
    char cwd[PATH_MAX];
    if (getcwd( cwd, sizeof cwd ) && !under( cwd, root, rootlen ) && !under( cwd, mac, maclen )) chdir( root );
}

// bit i set: argument i is a path name
static unsigned path_args( long nr, const long *a )
{
    switch (nr)
    {
    case NR_openat2:
        return a[2] && (((const uint64_t *)a[2])[2] & 0x18) ? 0 : 2;   // RESOLVE_IN_ROOT/BENEATH: FEX's own lookup
    case NR_openat: case NR_newfstatat: case NR_readlinkat: case NR_faccessat: case NR_faccessat2:
    case NR_statx: case NR_unlinkat: case NR_mkdirat: case NR_execveat: case NR_fchmodat:
    case NR_fchownat: case NR_utimensat: case NR_mknodat: case NR_inotify_add_watch:
        return 2;
    case NR_chdir: case NR_execve: case NR_truncate: case NR_statfs: case NR_getxattr: case NR_lgetxattr:
    case NR_setxattr: case NR_lsetxattr: case NR_listxattr: case NR_llistxattr: case NR_removexattr:
    case NR_lremovexattr:
        return 1;
    case NR_renameat: case NR_renameat2: case NR_linkat:
        return 2 | 8;
    case NR_symlinkat:   // the target is stored as written; FEX reads it back inside the root
        return 4;
    }
    return 0;
}

void inv_container_args( long nr, long *args, char (*bufs)[PATH_MAX] )
{
    unsigned mask = path_args( nr, args );
    for (int i = 0, k = 0; mask; i++, mask >>= 1)
        if (mask & 1) args[i] = (long)inv_container_path( (const char *)args[i], bufs[k++] );
}

void inv_container_after( long nr, const long *args, long r )
{
    if (r < 0) return;
    if (nr == NR_getcwd) to_guest( (char *)args[0], (size_t)args[1] );
    else if (nr == NR_chdir || nr == NR_fchdir) stay_inside();
}

// FEX implements many x86-64 calls with libc (::mkdir, ::rename, ::chmod...) and falls back to
// libc when a file isn't in the root; those get the same treatment as the raw calls. The mask
// says which arguments are paths; symlink's target is stored as written.
static struct { const char *name; unsigned paths; void *next; } libc_paths[] = {
    { "open", 1, 0 }, { "openat", 2, 0 }, { "stat", 1, 0 }, { "lstat", 1, 0 }, { "access", 1, 0 },
    { "faccessat", 2, 0 }, { "fstatat", 2, 0 }, { "readlink", 1, 0 }, { "readlinkat", 2, 0 }, { "mkdir", 1, 0 },
    { "rmdir", 1, 0 }, { "unlink", 1, 0 }, { "unlinkat", 2, 0 }, { "rename", 3, 0 }, { "link", 3, 0 },
    { "symlink", 2, 0 }, { "chmod", 1, 0 }, { "fchmodat", 2, 0 }, { "chown", 1, 0 }, { "lchown", 1, 0 },
    { "mknod", 1, 0 }, { "mknodat", 2, 0 }, { "truncate", 1, 0 }, { "statfs", 1, 0 }, { "utimes", 1, 0 },
    { "utime", 1, 0 }, { "getxattr", 1, 0 }, { "lgetxattr", 1, 0 }, { "setxattr", 1, 0 }, { "lsetxattr", 1, 0 },
    { "listxattr", 1, 0 }, { "llistxattr", 1, 0 }, { "removexattr", 1, 0 }, { "lremovexattr", 1, 0 },
};
enum { NWRAP = sizeof libc_paths / sizeof libc_paths[0] };

static long through( int i, long a, long b, long c, long d, long e, long f )
{
    long args[6] = { a, b, c, d, e, f };
    char bufs[2][PATH_MAX];
    for (int k = 0, n = 0; k < 6; k++)
        if (libc_paths[i].paths >> k & 1) args[k] = (long)inv_container_path( (const char *)args[k], bufs[n++] );
    return ((long (*)( long, long, long, long, long, long ))libc_paths[i].next)( args[0], args[1], args[2], args[3], args[4], args[5] );
}
#define W( i ) static long w##i( long a, long b, long c, long d, long e, long f ) { return through( i, a, b, c, d, e, f ); }
W(0) W(1) W(2) W(3) W(4) W(5) W(6) W(7) W(8) W(9) W(10) W(11) W(12) W(13) W(14) W(15) W(16) W(17) W(18) W(19)
W(20) W(21) W(22) W(23) W(24) W(25) W(26) W(27) W(28) W(29) W(30) W(31) W(32) W(33)
static void *const wrappers[] = { w0, w1, w2, w3, w4, w5, w6, w7, w8, w9, w10, w11, w12, w13, w14, w15, w16, w17, w18,
                                  w19, w20, w21, w22, w23, w24, w25, w26, w27, w28, w29, w30, w31, w32, w33 };
_Static_assert( sizeof wrappers / sizeof wrappers[0] == NWRAP, "one wrapper each" );

void *kl_shim_lookup( const char *name );
static int looking;

// the function a name means without the wrappers: the glibc layer's, else Klepton's shim
void *inv_libc_plain( const char *name )
{
    looking++;
    void *p = mvr_glibc_lookup( name );
    if (!p) p = kl_shim_lookup( name );
    looking--;
    return p;
}

// FEX's imports: the wrapper, around what the name would have bound to
void *inv_container_lookup( const char *name )
{
    if (!inv_container || looking) return NULL;
    for (int i = 0; i < NWRAP; i++)
    {
        if (strcmp( libc_paths[i].name, name )) continue;
        if (!libc_paths[i].next) libc_paths[i].next = inv_libc_plain( name );
        return libc_paths[i].next ? wrappers[i] : NULL;
    }
    return NULL;
}
