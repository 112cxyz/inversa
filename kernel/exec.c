// execve/execveat. Inversa isn't a binfmt_misc interpreter, so a guest exec of a Linux program
// re-runs Inversa itself: `inversa run <program> args...`. FEX execs its children as
// execveat(dirfd, "/proc/self/exe", ["FEX", program, args...]), expecting /proc/self/exe to be
// FEX; here that is Inversa, so the "FEX" in front becomes "inversa run".
#include "kernel.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int is_linux_elf( const char *path )
{
    unsigned char h[20];
    int fd = open( path, O_RDONLY | O_CLOEXEC );
    if (fd < 0) return 0;
    ssize_t n = read( fd, h, sizeof h );
    close( fd );
    if (n < 20 || memcmp( h, "\177ELF", 4 )) return 0;
    int machine = h[18] | h[19] << 8;
    return machine == 62 || machine == 183;   // x86-64, AArch64
}

static long run_inversa( char *const *rest, char *const *envp )
{
    char self[PATH_MAX]; uint32_t n = sizeof self;
    if (_NSGetExecutablePath( self, &n )) { errno = ENOEXEC; return -1; }
    size_t count = 0;
    while (rest[count]) count++;
    char **argv = calloc( count + 3, sizeof *argv );
    if (!argv) { errno = ENOMEM; return -1; }
    argv[0] = "inversa"; argv[1] = "run";
    memcpy( argv + 2, rest, count * sizeof *argv );
    // already a 4K-page process: start the child in one too, rather than exec'ing normally and
    // having it re-exec itself (a second load of everything)
    if (getpagesize() == 0x1000)
    {
        posix_spawnattr_t attr;
        posix_spawnattr_init( &attr );
        posix_spawnattr_setflags( &attr, POSIX_SPAWN_SETEXEC );
        posix_spawnattr_set_4k_page_size_np( &attr );
        posix_spawn( NULL, self, NULL, &attr, argv, envp );
        posix_spawnattr_destroy( &attr );
    }
    execve( self, argv, envp );
    int e = errno;
    free( argv );
    errno = e;
    return -1;
}

long inv_execveat( long dirfd, const char *path, char *const *argv, char *const *envp, long flags )
{
    if (!path || !argv) { errno = EFAULT; return -1; }
    if (!strcmp( path, "/proc/self/exe" ))
    {
        if (!argv[0] || !argv[1]) { errno = EINVAL; return -1; }
        return run_inversa( argv + 1, envp );   // drop FEX's own argv[0]
    }
    char full[PATH_MAX];
    if ((flags & 0x1000) && !*path)   // AT_EMPTY_PATH: the fd itself
    {
        if (fcntl( (int)dirfd, F_GETPATH, full ) < 0) return -1;
    }
    else if (path[0] != '/' && (int)dirfd != -100)
    {
        char dir[PATH_MAX];
        if (fcntl( (int)dirfd, F_GETPATH, dir ) < 0) return -1;
        snprintf( full, sizeof full, "%s/%s", dir, path );
    }
    else snprintf( full, sizeof full, "%s", path );
    if (is_linux_elf( full ))
    {
        // the program's own argv[0] goes; inversa run puts the path there
        size_t count = 0;
        while (argv[count]) count++;
        char **rest = calloc( count + 2, sizeof *rest );
        if (!rest) { errno = ENOMEM; return -1; }
        rest[0] = full;
        for (size_t i = 1; i < count; i++) rest[i] = argv[i];
        long r = run_inversa( rest, envp );
        free( rest );
        return r;
    }
    return execve( full, argv, envp );
}
