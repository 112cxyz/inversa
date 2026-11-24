// inversa bash: a Linux shell in a root of its own, the way WSL keeps a distro. A container is
// an APFS clone of rootfs/x86_64 in ~/Library/Application Support/Inversa/containers/<name>,
// made the first time it's used and kept after. Inside it / is that directory, the Mac home
// folder is /mnt/mac and the shell starts in /home/<you>; kernel/container.c does the mapping.
#include "commands.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <pwd.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/clonefile.h>
#include <sys/stat.h>
#include <unistd.h>
#include <copyfile.h>

extern char **environ;

int inv_exec_self( char **argv, char **envp )
{
    char self[PATH_MAX]; uint32_t n = sizeof self;
    if (_NSGetExecutablePath( self, &n )) return -1;
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
    return -1;
}

static char *slurp( const char *path, size_t *len )
{
    FILE *f = fopen( path, "rb" );
    if (!f) return NULL;
    fseek( f, 0, SEEK_END );
    long n = ftell( f );
    rewind( f );
    char *s = malloc( (size_t)n + 1 );
    *len = fread( s, 1, (size_t)n, f );
    s[*len] = 0;
    fclose( f );
    return s;
}

// drop the lines of an /etc/passwd- or group-style file whose field `field` is `value`, then add `line`
static void replace_entry( const char *path, int field, const char *value, const char *line )
{
    size_t len = 0;
    char *old = slurp( path, &len ), *save = NULL;
    FILE *out = fopen( path, "w" );
    if (!out) { free( old ); return; }
    for (char *l = old ? strtok_r( old, "\n", &save ) : NULL; l; l = strtok_r( NULL, "\n", &save ))
    {
        const char *f = l;
        for (int i = 0; i < field && f; i++) { f = strchr( f, ':' ); if (f) f++; }
        size_t vl = strlen( value );
        if (f && !strncmp( f, value, vl ) && (f[vl] == ':' || f[vl] == 0)) continue;
        fprintf( out, "%s\n", l );
    }
    fprintf( out, "%s\n", line );
    fclose( out );
    free( old );
}

static void write_file( const char *path, const char *text )
{
    FILE *f = fopen( path, "w" );
    if (f) { fputs( text, f ); fclose( f ); }
}

static void copy_into( const char *from, const char *to )
{
    copyfile( from, to, NULL, COPYFILE_ALL );
}

// you, as the guest sees you: the same uid and gid, so files on /mnt/mac are yours
static void add_user( const char *root, const struct passwd *pw, const char *name )
{
    char path[PATH_MAX], line[512], id[32];
    snprintf( path, sizeof path, "%s/etc/passwd", root );
    snprintf( id, sizeof id, "%u", pw->pw_uid );
    snprintf( line, sizeof line, "%s:x:%u:%u:%s:/home/%s:/bin/bash", pw->pw_name, pw->pw_uid, pw->pw_gid, pw->pw_name, pw->pw_name );
    replace_entry( path, 2, id, line );
    snprintf( path, sizeof path, "%s/etc/group", root );
    snprintf( id, sizeof id, "%u", pw->pw_gid );
    snprintf( line, sizeof line, "staff:x:%u:%s", pw->pw_gid, pw->pw_name );
    replace_entry( path, 2, id, line );

    char home[PATH_MAX], skel[PATH_MAX], dst[PATH_MAX];
    snprintf( home, sizeof home, "%s/home/%s", root, pw->pw_name );
    mkdir( home, 0755 );
    for (const char **f = (const char *[]){ ".bashrc", ".profile", ".bash_logout", NULL }; *f; f++)
    {
        snprintf( skel, sizeof skel, "%s/etc/skel/%s", root, *f );
        snprintf( dst, sizeof dst, "%s/%s", home, *f );
        copy_into( skel, dst );
    }
    // FEX makes this itself, but its std::filesystem walk fails on the way down
    snprintf( path, sizeof path, "%s/.config", home );
    mkdir( path, 0755 );
    snprintf( path, sizeof path, "%s/.config/fex-emu", home );
    mkdir( path, 0755 );
    snprintf( path, sizeof path, "%s/mnt", root );
    mkdir( path, 0755 );
    // Debian's bashrc shows this in the prompt: (name)you@host:~$
    snprintf( path, sizeof path, "%s/etc/debian_chroot", root );
    snprintf( line, sizeof line, "%s\n", name );
    write_file( path, line );
    snprintf( path, sizeof path, "%s/etc/profile", root );
    if (access( path, R_OK ))
        write_file( path,
            "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin\n"
            "export PATH\n"
            "[ -n \"$BASH\" ] && [ -f /etc/bash.bashrc ] && . /etc/bash.bashrc\n" );
}

static int create( const char *root, const char *base, const char *lib, const struct passwd *pw, const char *name )
{
    char src[PATH_MAX], cand[PATH_MAX], tmp[PATH_MAX];
    snprintf( cand, sizeof cand, "%s/../rootfs/x86_64", lib );
    if (!realpath( cand, src ) || access( src, R_OK ))
    {
        fprintf( stderr, "inversa: no x86-64 root at %s to make the container from (make rootfs)\n", cand );
        return -1;
    }
    char dir[PATH_MAX];
    snprintf( dir, sizeof dir, "%s", base );
    for (char *p = dir + 1; *p; p++) if (*p == '/') { *p = 0; mkdir( dir, 0755 ); *p = '/'; }
    mkdir( dir, 0755 );
    snprintf( tmp, sizeof tmp, "%s/.%s.new", base, name );
    copyfile_state_t none = NULL;
    if (clonefile( src, tmp, 0 ) &&
        copyfile( src, tmp, none, COPYFILE_ALL | COPYFILE_RECURSIVE ))   // another volume: a real copy
    {
        fprintf( stderr, "inversa: copying %s to %s: %s\n", src, tmp, strerror( errno ) );
        return -1;
    }
    add_user( tmp, pw, name );
    if (rename( tmp, root )) { fprintf( stderr, "inversa: %s: %s\n", root, strerror( errno ) ); return -1; }
    fprintf( stderr, "inversa: made container \"%s\" at %s\n", name, root );
    return 0;
}

static int list( const char *base )
{
    char cmd[PATH_MAX + 16];
    snprintf( cmd, sizeof cmd, "ls -1 '%s' 2>/dev/null", base );
    return system( cmd ) ? 1 : 0;
}

int inv_bash( int argc, char **argv, const char *lib )
{
    const char *name = "default";
    int i = 0;
    for (; i < argc; i++)
    {
        if ((!strcmp( argv[i], "-n" ) || !strcmp( argv[i], "--name" )) && i + 1 < argc) name = argv[++i];
        else if (!strcmp( argv[i], "--list" )) name = NULL;
        else if (!strcmp( argv[i], "--" )) { i++; break; }
        else break;
    }
    const struct passwd *pw = getpwuid( getuid() );
    if (!pw) { fprintf( stderr, "inversa: who are you? (no passwd entry)\n" ); return 1; }
    char base[PATH_MAX], root[PATH_MAX];
    snprintf( base, sizeof base, "%s/Library/Application Support/Inversa/containers", pw->pw_dir );
    if (!name) return list( base );
    if (!*name || strchr( name, '/' ) || name[0] == '.') { fprintf( stderr, "inversa: bad container name \"%s\"\n", name ); return 2; }
    snprintf( root, sizeof root, "%s/%s", base, name );
    char bash[PATH_MAX];
    snprintf( bash, sizeof bash, "%s/usr/bin/bash", root );
    if (access( bash, X_OK ) && create( root, base, lib, pw, name )) return 1;

    char home[PATH_MAX];
    snprintf( home, sizeof home, "%s/home/%s", root, pw->pw_name );
    if (chdir( home ) && chdir( root )) { perror( root ); return 1; }

    // a Linux environment, not the Mac's: only the terminal and Inversa's own switches come in
    char *env[64]; int e = 0;
    static char vars[16][PATH_MAX + 32];
    int v = 0;
#define SET( ... ) (snprintf( vars[v], sizeof vars[v], __VA_ARGS__ ), env[e++] = vars[v++])
    SET( "HOME=/home/%s", pw->pw_name );
    SET( "USER=%s", pw->pw_name );
    SET( "LOGNAME=%s", pw->pw_name );
    SET( "PWD=/home/%s", pw->pw_name );
    SET( "INVERSA_CONTAINER=%s", root );
    SET( "INVERSA_CONTAINER_MAC=%s", pw->pw_dir );
    SET( "FEX_ROOTFS=%s", root );
    env[e++] = "SHELL=/bin/bash";
    env[e++] = "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
    env[e++] = "LANG=C.UTF-8";
    for (char **p = environ; *p && e < 60; p++)
        if (!strncmp( *p, "TERM=", 5 ) || !strncmp( *p, "COLORTERM=", 10 ) || !strncmp( *p, "INVERSA_", 8 ) ||
            !strncmp( *p, "FEX_", 4 ) || !strncmp( *p, "MVR_", 4 ) || !strncmp( *p, "KL_", 3 ))
        {
            if (!strncmp( *p, "INVERSA_CONTAINER", 17 ) || !strncmp( *p, "FEX_ROOTFS=", 11 )) continue;
            env[e++] = *p;
        }
    // the loader's reports, at every exec, are no use in a shell
    if (!getenv( "KL_TRACE_NEEDED" )) env[e++] = "KL_TRACE_NEEDED=0";
    if (!getenv( "KL_TRACE_PROC" )) env[e++] = "KL_TRACE_PROC=0";
    env[e] = NULL;
#undef SET

    char **args = calloc( (size_t)(argc - i) + 5, sizeof *args );
    int k = 0;
    args[k++] = "inversa";
    args[k++] = "run";
    args[k++] = "/usr/bin/bash";
    if (i == argc) args[k++] = "-l";
    for (; i < argc; i++) args[k++] = argv[i];
    inv_exec_self( args, env );
    fprintf( stderr, "inversa: starting bash: %s\n", strerror( errno ) );
    return 1;
}
