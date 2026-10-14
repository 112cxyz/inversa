// Steam for Linux game builds: the real macOS Steam client. Valve's Linux libsteam_api
// finds a running client through ~/.steam/steam.pid and dlopens
// ~/.steam/sdkarm64/steamclient.so; that dlopen is answered with the arm64 slice of the
// macOS steamclient.dylib (same C++ interfaces, Itanium ABI on both sides), which talks
// to the macOS Steam process. Steam.app must be running and signed in by the game's owner.
#include "klepton.h"
#include <dlfcn.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>

static void *steamclient;

static long noop(void) { return 0; }

static void *steamclient_sym( const char *name )
{
    /* Steam's Breakpad takes the Mach exception port, turning every fault into a silent
     * minidump-and-exit; crash reporting isn't needed to reach Steam */
    if (!strncmp( name, "Breakpad_", 9 )) return (void *)noop;
    return steamclient ? dlsym( steamclient, name ) : NULL;
}

static pid_t steam_pid(void)
{
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_ALL, 0 };
    size_t len = 0;
    if (sysctl( mib, 4, NULL, &len, NULL, 0 )) return 0;
    struct kinfo_proc *procs = malloc( len );
    pid_t found = 0;
    if (procs && !sysctl( mib, 4, procs, &len, NULL, 0 ))
        for (size_t i = 0; i < len / sizeof *procs; i++)
            if (!strcmp( procs[i].kp_proc.p_comm, "steam_osx" )) { found = procs[i].kp_proc.p_pid; break; }
    free( procs );
    return found;
}

void mvr_steam_init( const char *appid )
{
    const char *home = getenv( "HOME" );
    if (!home || getenv( "MVR_NO_STEAM" )) return;
    char path[PATH_MAX];
    snprintf( path, sizeof path, "%s/.steam", home ); mkdir( path, 0755 );
    snprintf( path, sizeof path, "%s/.steam/sdkarm64", home ); mkdir( path, 0755 );

    pid_t pid = steam_pid();
    snprintf( path, sizeof path, "%s/.steam/steam.pid", home );
    FILE *f = fopen( path, "w" );
    if (f) { fprintf( f, "%d\n", pid ? pid : getpid() ); fclose( f ); }
    snprintf( path, sizeof path, "%s/.steam/sdkarm64/steamclient.so", home );
    f = fopen( path, "a" ); if (f) fclose( f );     /* steam_api checks the path exists */

    const char *dylib = getenv( "MVR_STEAMCLIENT" );
    char def[PATH_MAX];
    snprintf( def, sizeof def, "%s/Library/Application Support/Steam/Steam.AppBundle/Steam/Contents/MacOS/steamclient.dylib", home );
    steamclient = dlopen( dylib ? dylib : def, RTLD_NOW | RTLD_LOCAL );
    fprintf( stderr, "[mvr] steam: macOS steamclient %s, Steam.app %s\n", steamclient ? "loaded" : dlerror(),
             pid ? "running" : "NOT running - start Steam and sign in" );
    if (appid) { setenv( "SteamAppId", appid, 0 ); setenv( "SteamGameId", appid, 0 ); }
    kl_dl_add_host_module( "/steamclient.so", steamclient_sym );
}
