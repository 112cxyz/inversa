// inversa apk: Android APKs (Quest games, mostly) through Klepton, in a window or in the headset
// through MoltenVR. The APK is a local file, or a package name pulled off a Quest over adb with
// its split APKs and OBBs. Each one is unpacked once with apktool into
// ~/Library/Application Support/Inversa/apps/<name>/, beside <name>.apk, which is the layout
// Klepton's frontend (Contents/MacOS/klepton, its m_boot) expects; the game's saves and OBBs live
// in Klepton's userdata folder for <name>.
#include "commands.h"
#include <ctype.h>
#include <stdarg.h>
#include <dirent.h>
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
#include <sys/wait.h>
#include <unistd.h>
#include <copyfile.h>

extern char **environ;

static char g_adb[PATH_MAX], g_serial[64];

// argv run to completion; with out, its stdout collected there. Returns the exit status
static int run( char *const *argv, char *out, size_t cap )
{
    int fds[2] = { -1, -1 };
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init( &fa );
    if (out)
    {
        if (pipe( fds )) return -1;
        posix_spawn_file_actions_adddup2( &fa, fds[1], 1 );
        posix_spawn_file_actions_addclose( &fa, fds[0] );
    }
    pid_t pid;
    int err = posix_spawnp( &pid, argv[0], &fa, NULL, argv, environ );
    posix_spawn_file_actions_destroy( &fa );
    if (out) close( fds[1] );
    if (err) { if (out) close( fds[0] ); errno = err; return -1; }
    if (out)
    {
        size_t n = 0;
        ssize_t r;
        while ((r = read( fds[0], out + n, n + 1 < cap ? cap - n - 1 : 0 )) > 0) n += (size_t)r;
        out[n] = 0;
        close( fds[0] );
    }
    int st = 0;
    while (waitpid( pid, &st, 0 ) < 0 && errno == EINTR) {}
    return WIFEXITED( st ) ? WEXITSTATUS( st ) : 128 + WTERMSIG( st );
}

static int exists( const char *path ) { return !access( path, F_OK ); }

static void mkdirs( const char *path )
{
    char dir[PATH_MAX];
    snprintf( dir, sizeof dir, "%s", path );
    for (char *p = dir + 1; *p; p++) if (*p == '/') { *p = 0; mkdir( dir, 0755 ); *p = '/'; }
    mkdir( dir, 0755 );
}

static int is_package( const char *s )
{
    if (!strchr( s, '.' ) || strchr( s, '/' )) return 0;
    for (; *s; s++) if (!isalnum( (unsigned char)*s ) && *s != '.' && *s != '_') return 0;
    return 1;
}

static const char *find_tool( const char *name, const char *const *also )
{
    static char found[PATH_MAX];
    const char *path = getenv( "PATH" );
    char buf[4096];
    snprintf( buf, sizeof buf, "%s:/opt/homebrew/bin:/usr/local/bin", path ? path : "/usr/bin:/bin" );
    for (char *save, *d = strtok_r( buf, ":", &save ); d; d = strtok_r( NULL, ":", &save ))
    {
        snprintf( found, sizeof found, "%s/%s", d, name );
        if (!access( found, X_OK )) return found;
    }
    for (; also && *also; also++) if (!access( *also, X_OK )) return *also;
    return NULL;
}

// adb [-s serial] args...
static int adb( char *out, size_t cap, ... )
{
    char *argv[16];
    int k = 0;
    argv[k++] = g_adb;
    if (*g_serial) { argv[k++] = "-s"; argv[k++] = g_serial; }
    va_list ap;
    va_start( ap, cap );
    for (char *a; (a = va_arg( ap, char * )) && k < 15;) argv[k++] = a;
    va_end( ap );
    argv[k] = NULL;
    return run( argv, out, cap );
}

static int adb_ready(void)
{
    const char *home = getenv( "HOME" );
    char sdk[PATH_MAX];
    snprintf( sdk, sizeof sdk, "%s/Library/Android/sdk/platform-tools/adb", home ? home : "" );
    const char *also[] = { sdk, NULL };
    const char *a = find_tool( "adb", also );
    if (!a) { fprintf( stderr, "inversa: no adb (brew install android-platform-tools)\n" ); return 0; }
    snprintf( g_adb, sizeof g_adb, "%s", a );
    char out[4096];
    if (adb( out, sizeof out, "get-state", NULL ) || !strstr( out, "device" ))
    {
        char list[4096] = "";
        char *argv[] = { g_adb, "devices", NULL };
        run( argv, list, sizeof list );
        int n = 0;
        for (char *p = list; (p = strstr( p, "\tdevice" )); p++) n++;
        if (n > 1 && !*g_serial) fprintf( stderr, "inversa: %d devices on adb; pick the Quest with -s <serial> (adb devices lists them)\n", n );
        else fprintf( stderr, "inversa: no Quest on adb. Turn on developer mode, plug it in and allow USB debugging "
                              "in the headset (adb devices should list it)\n" );
        return 0;
    }
    char model[128] = "", maker[128] = "";
    adb( model, sizeof model, "shell", "getprop", "ro.product.model", NULL );
    adb( maker, sizeof maker, "shell", "getprop", "ro.product.manufacturer", NULL );
    model[strcspn( model, "\r\n" )] = 0;
    maker[strcspn( maker, "\r\n" )] = 0;
    fprintf( stderr, "inversa: using %s %s over adb%s\n", maker, model,
             strcasestr( maker, "oculus" ) || strcasestr( maker, "meta" ) ? "" : " (not a Quest, but any arm64 Android works)" );
    return 1;
}

static int list_quest(void)
{
    if (!adb_ready()) return 1;
    static char out[1 << 18];
    if (adb( out, sizeof out, "shell", "pm", "list", "packages", "-3", NULL )) { fprintf( stderr, "inversa: pm list failed\n" ); return 1; }
    for (char *save, *l = strtok_r( out, "\r\n", &save ); l; l = strtok_r( NULL, "\r\n", &save ))
        if (!strncmp( l, "package:", 8 )) printf( "%s\n", l + 8 );
    return 0;
}

// size of a file on the headset, -1 if it isn't there
static long long remote_size( const char *path )
{
    char out[256];
    if (adb( out, sizeof out, "shell", "stat", "-c", "%s", path, NULL )) return -1;
    return atoll( out );
}

static int pull( const char *remote, const char *local )
{
    struct stat st;
    long long size = remote_size( remote );
    if (size >= 0 && !stat( local, &st ) && st.st_size == size) return 0;   // already here
    fprintf( stderr, "inversa: pulling %s\n", remote );
    char tmp[PATH_MAX];
    snprintf( tmp, sizeof tmp, "%s.part", local );
    char *argv_r = (char *)remote, *argv_l = tmp;
    if (adb( NULL, 0, "pull", argv_r, argv_l, NULL )) { fprintf( stderr, "inversa: adb pull %s failed\n", remote ); return -1; }
    return rename( tmp, local );
}

// the package's APKs (base and splits) into apps/<pkg>.apk and apps/<pkg>.splits/
static int pull_package( const char *pkg, const char *apps )
{
    static char out[1 << 16];
    char p[PATH_MAX];
    if (adb( out, sizeof out, "shell", "pm", "path", pkg, NULL ) || !strstr( out, "package:" ))
    {
        fprintf( stderr, "inversa: %s isn't installed on the Quest (inversa apk --quest lists what is)\n", pkg );
        return -1;
    }
    for (char *save, *l = strtok_r( out, "\r\n", &save ); l; l = strtok_r( NULL, "\r\n", &save ))
    {
        if (strncmp( l, "package:", 8 )) continue;
        const char *remote = l + 8, *base = strrchr( remote, '/' );
        base = base ? base + 1 : remote;
        if (!strcmp( base, "base.apk" )) snprintf( p, sizeof p, "%s/%s.apk", apps, pkg );
        else
        {
            snprintf( p, sizeof p, "%s/%s.splits", apps, pkg );
            mkdir( p, 0755 );
            snprintf( p, sizeof p, "%s/%s.splits/%s", apps, pkg, base );
        }
        if (pull( remote, p )) return -1;
    }
    return 0;
}

// OBBs from /sdcard/Android/obb/<pkg>/ into the folder the game will look in
static void pull_obbs( const char *pkg, const char *dest )
{
    char dir[256], out[8192], remote[512], local[PATH_MAX];
    snprintf( dir, sizeof dir, "/sdcard/Android/obb/%s", pkg );
    if (adb( out, sizeof out, "shell", "ls", "-1", dir, NULL )) return;
    for (char *save, *l = strtok_r( out, "\r\n", &save ); l; l = strtok_r( NULL, "\r\n", &save ))
    {
        if (!strstr( l, ".obb" ) || strchr( l, '/' )) continue;
        mkdirs( dest );
        snprintf( remote, sizeof remote, "%s/%s", dir, l );
        snprintf( local, sizeof local, "%s/%s", dest, l );
        pull( remote, local );
    }
}

// OBBs that came beside a local APK
static void copy_obbs( const char *apk, const char *dest )
{
    char dir[PATH_MAX], from[PATH_MAX], to[PATH_MAX];
    snprintf( dir, sizeof dir, "%s", apk );
    char *slash = strrchr( dir, '/' );
    if (slash) *slash = 0; else snprintf( dir, sizeof dir, "." );
    DIR *d = opendir( dir );
    if (!d) return;
    for (struct dirent *e; (e = readdir( d ));)
    {
        size_t n = strlen( e->d_name );
        if (n < 5 || strcmp( e->d_name + n - 4, ".obb" )) continue;
        snprintf( from, sizeof from, "%s/%s", dir, e->d_name );
        snprintf( to, sizeof to, "%s/%s", dest, e->d_name );
        if (exists( to )) continue;
        mkdirs( dest );
        fprintf( stderr, "inversa: copying %s\n", e->d_name );
        if (clonefile( from, to, 0 )) copyfile( from, to, NULL, COPYFILE_ALL );
    }
    closedir( d );
}

static char *slurp( const char *path )
{
    FILE *f = fopen( path, "rb" );
    if (!f) return NULL;
    fseek( f, 0, SEEK_END );
    long n = ftell( f );
    rewind( f );
    char *s = malloc( (size_t)n + 1 );
    size_t got = fread( s, 1, (size_t)n, f );
    s[got] = 0;
    fclose( f );
    return s;
}

// attr="value" after `from` in the manifest, into out
static int attr( const char *from, const char *name, char *out, size_t cap )
{
    char key[64];
    snprintf( key, sizeof key, "%s=\"", name );
    const char *p = from ? strstr( from, key ) : NULL;
    if (!p) return 0;
    p += strlen( key );
    size_t n = strcspn( p, "\"" );
    if (n >= cap) n = cap - 1;
    memcpy( out, p, n );
    out[n] = 0;
    return 1;
}

typedef struct { const char *kind; char entry[128]; char obb[256]; char package[200]; } guest;

// what Klepton needs to know to start it: the engine, from the libraries, else the manifest's activity
static int identify( const char *tree, guest *g )
{
    char path[PATH_MAX];
    snprintf( path, sizeof path, "%s/AndroidManifest.xml", tree );
    char *manifest = slurp( path );
    if (!manifest) { fprintf( stderr, "inversa: %s has no decoded manifest\n", tree ); return -1; }
    attr( strstr( manifest, "<manifest" ), "package", g->package, sizeof g->package );
    snprintf( g->obb, sizeof g->obb, "obb" );
#define LIB( name ) (snprintf( path, sizeof path, "%s/lib/arm64-v8a/" name, tree ), exists( path ))
    if (LIB( "libunity.so" )) { g->kind = "unity"; snprintf( g->entry, sizeof g->entry, "libmain" ); }
    else if (LIB( "libUE4.so" ))
    {
        g->kind = "ue4";
        snprintf( g->entry, sizeof g->entry, "libUE4" );
        snprintf( g->obb, sizeof g->obb, "Android/obb/%s", g->package );
    }
    else if (strstr( manifest, "android.app.NativeActivity" ))
    {
        g->kind = "native";
        char lib[100] = "main";
        const char *meta = strstr( manifest, "android.app.lib_name" );
        if (meta) { while (meta > manifest && *meta != '<') meta--; attr( meta, "android:value", lib, sizeof lib ); }
        snprintf( g->entry, sizeof g->entry, "lib%s", lib );
    }
    else if (strstr( manifest, "org.libsdl.app.SDLActivity" ) || strstr( manifest, "SDLActivity" ))
    {
        g->kind = "sdl2";
        snprintf( g->entry, sizeof g->entry, "libmain" );
    }
    else if (strstr( manifest, "GLES3JNI" )) g->kind = "gles3jni";
    free( manifest );
#undef LIB
    snprintf( path, sizeof path, "%s/lib/arm64-v8a", tree );
    if (!exists( path ))
    {
        fprintf( stderr, "inversa: %s has no 64-bit ARM libraries (lib/arm64-v8a): nothing Klepton can run\n", tree );
        return -1;
    }
    return 0;
}

static int unpack( const char *apps, const char *name )
{
    char apk[PATH_MAX], tree[PATH_MAX], manifest[PATH_MAX];
    snprintf( apk, sizeof apk, "%s/%s.apk", apps, name );
    snprintf( tree, sizeof tree, "%s/%s", apps, name );
    snprintf( manifest, sizeof manifest, "%s/AndroidManifest.xml", tree );
    struct stat a, m;
    if (!stat( manifest, &m ) && !stat( apk, &a ) && m.st_mtime >= a.st_mtime) return 0;
    const char *apktool = find_tool( "apktool", NULL );
    if (!apktool) { fprintf( stderr, "inversa: unpacking needs apktool (brew install apktool)\n" ); return -1; }
    fprintf( stderr, "inversa: unpacking %s.apk\n", name );
    // -s: the dex stays packed; Klepton doesn't run Java
    char *argv[] = { (char *)apktool, "d", "-f", "-s", "-o", tree, apk, NULL };
    if (run( argv, NULL, 0 )) { fprintf( stderr, "inversa: apktool couldn't unpack %s\n", apk ); return -1; }

    // split APKs carry the native libraries and asset packs of bundle-built games
    char splits[PATH_MAX];
    snprintf( splits, sizeof splits, "%s/%s.splits", apps, name );
    DIR *d = opendir( splits );
    if (d)
    {
        for (struct dirent *e; (e = readdir( d ));)
        {
            if (!strstr( e->d_name, ".apk" )) continue;
            char split[PATH_MAX];
            snprintf( split, sizeof split, "%s/%s", splits, e->d_name );
            char *uz[] = { "unzip", "-o", "-q", split, "lib/arm64-v8a/*", "assets/*", "-d", tree, NULL };
            run( uz, NULL, 0 );   // 11: a split with neither, which is fine
        }
        closedir( d );
    }
    return 0;
}

// ANGLE for GLES games: KL_ANGLE_DIR, else one put in Inversa's folder, else the one Klepton's
// make angle-all leaves in a source checkout (lib/ is build/lib there)
static const char *angle_dir( const char *support, const char *lib )
{
    static char dir[PATH_MAX];
    const char *env = getenv( "KL_ANGLE_DIR" );
    if (env && *env) return env;
    char cand[2][PATH_MAX];
    snprintf( cand[0], PATH_MAX, "%s/angle", support );
    snprintf( cand[1], PATH_MAX, "%s/../../third_party/Klepton/vendor/out/Debug", lib );
    for (int i = 0; i < 2; i++)
    {
        snprintf( dir, sizeof dir, "%s/libEGL.dylib", cand[i] );
        if (exists( dir )) { snprintf( dir, sizeof dir, "%s", cand[i] ); return dir; }
    }
    return NULL;
}

static int usage(void)
{
    fprintf( stderr,
        "usage: inversa apk <file.apk | package> [--vr] [-s serial] [--kind K --entry libX] [--unpack-only]\n"
        "       inversa apk --quest           list the games on the connected Quest\n"
        "  A package name (com.example.game) is pulled off the Quest over adb, with its OBBs.\n"
        "  --vr plays it in the headset through MoltenVR; otherwise it opens in a window.\n" );
    return 2;
}

int inv_apk( int argc, char **argv, const char *lib )
{
    const char *target = NULL, *kind = NULL, *entry = NULL;
    int vr = 0, unpack_only = 0;
    for (int i = 0; i < argc; i++)
    {
        if (!strcmp( argv[i], "--vr" )) vr = 1;
        else if (!strcmp( argv[i], "--quest" )) return list_quest();
        else if (!strcmp( argv[i], "--unpack-only" )) unpack_only = 1;
        else if (!strcmp( argv[i], "-s" ) && i + 1 < argc) snprintf( g_serial, sizeof g_serial, "%s", argv[++i] );
        else if (!strcmp( argv[i], "--kind" ) && i + 1 < argc) kind = argv[++i];
        else if (!strcmp( argv[i], "--entry" ) && i + 1 < argc) entry = argv[++i];
        else if (argv[i][0] == '-') return usage();
        else if (!target) target = argv[i];
        else return usage();
    }
    if (!target) return usage();

    const struct passwd *pw = getpwuid( getuid() );
    if (!pw) return 1;
    char support[PATH_MAX], apps[PATH_MAX], name[200], apk[PATH_MAX];
    snprintf( support, sizeof support, "%s/Library/Application Support/Inversa", pw->pw_dir );
    snprintf( apps, sizeof apps, "%s/apps", support );
    mkdirs( apps );

    struct stat st;
    const char *local = NULL;
    if (!stat( target, &st ) && S_ISREG( st.st_mode ))
    {
        // a local APK: named after its file, copied in (a clone, on the same volume)
        const char *base = strrchr( target, '/' );
        base = base ? base + 1 : target;
        snprintf( name, sizeof name, "%s", base );
        char *dot = strrchr( name, '.' );
        if (dot && !strcasecmp( dot, ".apk" )) *dot = 0;
        for (char *p = name; *p; p++) if (!isalnum( (unsigned char)*p ) && *p != '.' && *p != '_' && *p != '-') *p = '_';
        snprintf( apk, sizeof apk, "%s/%s.apk", apps, name );
        char real_t[PATH_MAX], real_a[PATH_MAX];
        if (!realpath( target, real_t ) || !realpath( apk, real_a ) || strcmp( real_t, real_a ))
        {
            struct stat old;
            if (!stat( apk, &old ) && (old.st_size != st.st_size || old.st_mtime < st.st_mtime)) unlink( apk );
            if (!exists( apk ) && clonefile( target, apk, 0 ) && copyfile( target, apk, NULL, COPYFILE_ALL ))
            {
                fprintf( stderr, "inversa: copying %s: %s\n", target, strerror( errno ) );
                return 1;
            }
        }
        local = target;
    }
    else if (is_package( target ))
    {
        snprintf( name, sizeof name, "%s", target );
        snprintf( apk, sizeof apk, "%s/%s.apk", apps, name );
        if (!exists( apk ) || g_serial[0] || getenv( "INVERSA_APK_REPULL" ))
        {
            if (!adb_ready() || pull_package( target, apps )) return 1;
        }
    }
    else
    {
        fprintf( stderr, "inversa: %s is neither an APK file nor a package name\n", target );
        return 2;
    }

    if (unpack( apps, name )) return 1;
    char tree[PATH_MAX];
    snprintf( tree, sizeof tree, "%s/%s", apps, name );
    guest g = { 0 };
    if (identify( tree, &g )) return 1;
    if (kind) g.kind = kind;
    if (entry) snprintf( g.entry, sizeof g.entry, "%s", entry );
    if (!g.kind || !g.entry[0])
    {
        fprintf( stderr, "inversa: can't tell how %s starts (not Unity, Unreal, NativeActivity or SDL); "
                         "give --kind (unity, ue4, native, sdl2, gles3jni, jkxr) and --entry libX\n", name );
        return 1;
    }

    // OBBs go where the game will look: Klepton's userdata for this name, then the engine's own path
    char obbdir[PATH_MAX];
    snprintf( obbdir, sizeof obbdir, "%s/Library/Application Support/Klepton/userdata/%s/%s", pw->pw_dir, name, g.obb );
    if (local) copy_obbs( local, obbdir );
    else if (g_adb[0]) pull_obbs( g.package[0] ? g.package : name, obbdir );

    fprintf( stderr, "inversa: %s (%s, %s, starts at %s)%s\n", name, g.package[0] ? g.package : "?", g.kind, g.entry,
             vr ? ", in the headset" : "" );
    if (unpack_only) return 0;

    char exe[PATH_MAX], klepton[PATH_MAX];
    uint32_t n = sizeof exe;
    if (_NSGetExecutablePath( exe, &n ) || !realpath( exe, klepton )) return 1;
    char *slash = strrchr( klepton, '/' );
    snprintf( slash + 1, sizeof klepton - (size_t)(slash + 1 - klepton), "klepton" );
    if (access( klepton, X_OK )) { fprintf( stderr, "inversa: no Klepton frontend at %s\n", klepton ); return 1; }

    setenv( "KL_TARGET_KIND", g.kind, 1 );
    setenv( "KL_TARGET_ENTRY", g.entry, 1 );
    setenv( "KL_TARGET_OBB", g.obb, 1 );
    setenv( "KL_LIFECYCLE", "1", 0 );   // without it the guest never pumps
    setenv( "KL_GLFB", "1", 0 );
    setenv( "KL_GLFB_EXPOSURE", "1.0", 0 );
    setenv( "KL_GLFB_GAMMA", "0.45", 0 );
    setenv( "KL_NET_OFFLINE", "0", 0 );
    const char *angle = angle_dir( support, lib );
    if (angle) setenv( "KL_ANGLE_DIR", angle, 0 );
    else fprintf( stderr, "inversa: no ANGLE build found, so GLES games won't draw (Vulkan ones will). "
                          "Build it with Klepton's make angle-all and put vendor/out/Debug at %s/angle\n", support );
    if (!getenv( "KL_MVK_DIR" ) && !getenv( "KL_MVK_DYLIB" ))
    {
        char fw[PATH_MAX];
        snprintf( fw, sizeof fw, "%s", klepton );
        *strrchr( fw, '/' ) = 0;
        strncat( fw, "/../Frameworks", sizeof fw - strlen( fw ) - 1 );
        char dylib[PATH_MAX];
        snprintf( dylib, sizeof dylib, "%s/libMoltenVK.dylib", fw );
        if (exists( dylib )) setenv( "KL_MVK_DIR", fw, 0 );
    }
    if (vr)
    {
        setenv( "KL_MVR", "1", 1 );
        char rt[PATH_MAX];
        const char *e = getenv( "MOLTENVR_OPENXR_LIB" );
        snprintf( rt, sizeof rt, "%s/Library/Application Support/MoltenVR/bin/libmoltenvr_openxr.dylib", pw->pw_dir );
        if (!(e && *e) && !exists( rt ))
        {
            fprintf( stderr, "inversa: --vr needs MoltenVR's OpenXR runtime (%s); open MoltenVR once to install it\n", rt );
            return 1;
        }
    }
    else
    {
        setenv( "KL_VIEW", "1", 1 );
        setenv( "KL_OVRP_HANDS_IN_VIEW", "1", 0 );
    }
    if (chdir( apps )) { perror( apps ); return 1; }
    char libdir[300];
    snprintf( libdir, sizeof libdir, "%s/lib/arm64-v8a", name );
    char *kargv[] = { klepton, libdir, NULL };
    execve( klepton, kargv, environ );
    fprintf( stderr, "inversa: starting %s: %s\n", klepton, strerror( errno ) );
    return 1;
}
