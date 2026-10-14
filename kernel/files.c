// openat2 with RESOLVE_IN_ROOT / RESOLVE_BENEATH: how FEX looks inside the guest's root
// filesystem. Darwin has no openat2, so the path is walked here a component at a time, keeping
// ".." and symlinks (absolute ones restart at the root) inside it.
#include "kernel.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void *mvr_glibc_lookup( const char *name );
void *kl_shim_lookup( const char *name );

enum { RESOLVE_NO_XDEV = 1, RESOLVE_NO_MAGICLINKS = 2, RESOLVE_NO_SYMLINKS = 4, RESOLVE_BENEATH = 8, RESOLVE_IN_ROOT = 16 };

// root + path -> a host path that stays inside root; 0 or an errno
static int resolve( const char *root, const char *path, int beneath, int nosym, char *out )
{
    char rest[PATH_MAX * 2], cur[PATH_MAX], comp[NAME_MAX + 1];
    size_t rootlen = strlen( root );
    snprintf( cur, sizeof cur, "%s", root );
    snprintf( rest, sizeof rest, "%s", path );
    if (beneath && rest[0] == '/') return EXDEV;
    int links = 0;
    char *p = rest;
    while (*p)
    {
        while (*p == '/') p++;
        if (!*p) break;
        size_t n = strcspn( p, "/" );
        if (n > NAME_MAX) return ENAMETOOLONG;
        memcpy( comp, p, n ); comp[n] = 0; p += n;
        if (!strcmp( comp, "." )) continue;
        if (!strcmp( comp, ".." ))
        {
            if (strlen( cur ) > rootlen) *strrchr( cur, '/' ) = 0;
            else if (beneath) return EXDEV;
            continue;
        }
        size_t cl = strlen( cur );
        if (cl + 1 + n >= sizeof cur) return ENAMETOOLONG;
        cur[cl] = '/'; memcpy( cur + cl + 1, comp, n + 1 );
        struct stat st;
        if (lstat( cur, &st ) || !S_ISLNK( st.st_mode )) continue;   // a missing last part is open's to report
        if (nosym) return ELOOP;
        if (++links > 40) return ELOOP;
        char target[PATH_MAX];
        ssize_t tl = readlink( cur, target, sizeof target - 1 );
        if (tl < 0) return errno;
        target[tl] = 0;
        cur[cl] = 0;                                   // the link's directory
        if (target[0] == '/')
        {
            if (beneath) return EXDEV;
            snprintf( cur, sizeof cur, "%s", root );   // absolute: from the root again
        }
        char next[PATH_MAX * 2];
        snprintf( next, sizeof next, "%s/%s", target, p );
        snprintf( rest, sizeof rest, "%s", next );
        p = rest;
    }
    snprintf( out, PATH_MAX, "%s", cur );
    return 0;
}

long inv_openat2( long dirfd, const char *path, const uint64_t *how, size_t size )
{
    if (size < 24) { errno = EINVAL; return -1; }
    uint64_t flags = how[0], mode = how[1], res = how[2];
    typedef int (*openat_fn)( int, const char *, int, int );
    openat_fn op = (openat_fn)mvr_glibc_lookup( "openat" );
    if (!op) op = (openat_fn)kl_shim_lookup( "openat" );
    if (!(res & (RESOLVE_IN_ROOT | RESOLVE_BENEATH)))
        return op( (int)dirfd, path, (int)flags, (int)mode );   // nothing to confine
    char root[PATH_MAX], host[PATH_MAX];
    if ((int)dirfd == -100) { if (!getcwd( root, sizeof root )) return -1; }
    else if (fcntl( (int)dirfd, F_GETPATH, root ) < 0) return -1;
    int e = resolve( root, path, (res & RESOLVE_BENEATH) && !(res & RESOLVE_IN_ROOT), (res & RESOLVE_NO_SYMLINKS) != 0, host );
    if (e) { errno = e; return -1; }
    return op( -100, host, (int)flags, (int)mode );
}

// fstatat into the arm64 Linux struct stat (asm-generic, 128 bytes): Darwin's is 144, and
// writing that into a Linux-sized buffer runs over the caller's frame
struct linux_stat {
    uint64_t dev, ino; uint32_t mode, nlink, uid, gid; uint64_t rdev, pad1; int64_t size;
    int32_t blksize, pad2; int64_t blocks; int64_t atime; uint64_t atime_ns; int64_t mtime; uint64_t mtime_ns;
    int64_t ctime; uint64_t ctime_ns; uint32_t unused[2];
};
_Static_assert( sizeof(struct linux_stat) == 128, "arm64 Linux struct stat" );

static void to_linux( const struct stat *s, struct linux_stat *l )
{
    *l = (struct linux_stat){ .dev = (uint64_t)s->st_dev, .ino = s->st_ino, .mode = s->st_mode, .nlink = s->st_nlink,
                              .uid = s->st_uid, .gid = s->st_gid, .rdev = (uint64_t)s->st_rdev, .size = s->st_size,
                              .blksize = s->st_blksize, .blocks = s->st_blocks,
                              .atime = s->st_atimespec.tv_sec, .atime_ns = (uint64_t)s->st_atimespec.tv_nsec,
                              .mtime = s->st_mtimespec.tv_sec, .mtime_ns = (uint64_t)s->st_mtimespec.tv_nsec,
                              .ctime = s->st_ctimespec.tv_sec, .ctime_ns = (uint64_t)s->st_ctimespec.tv_nsec };
}

long inv_fstatat( long dirfd, const char *path, void *buf, long flags )
{
    struct stat st;
    int fd = (int)dirfd == -100 ? AT_FDCWD : (int)dirfd;
    int r = (flags & 0x1000) && (!path || !*path) ? fstat( fd, &st )       // AT_EMPTY_PATH
                                                 : fstatat( fd, path, &st, (flags & 0x100) ? AT_SYMLINK_NOFOLLOW : 0 );
    if (r) return -1;
    to_linux( &st, buf );
    return 0;
}

long inv_fstat( long fd, void *buf )
{
    struct stat st;
    if (fstat( (int)fd, &st )) return -1;
    to_linux( &st, buf );
    return 0;
}
