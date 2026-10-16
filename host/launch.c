// The bundle's executable: put the real inversa (inversa-core, beside it) in a 4K-page process
// in one step. The core links Klepton's frameworks, which cost a few milliseconds to load each
// time; doing the 4K re-exec from here means they load once.
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <mach-o/dyld.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern char **environ;

static int entitled(void)
{
    SecTaskRef task = SecTaskCreateFromSelf( NULL );
    if (!task) return 0;
    CFTypeRef v = SecTaskCopyValueForEntitlement( task, CFSTR( "com.apple.developer.cross-architecture-support" ), NULL );
    int on = v && CFGetTypeID( v ) == CFBooleanGetTypeID() && CFBooleanGetValue( (CFBooleanRef)v );
    if (v) CFRelease( v );
    CFRelease( task );
    return on;
}

int main( int argc, char **argv )
{
    char core[1024]; uint32_t n = sizeof core;
    if (_NSGetExecutablePath( core, &n )) return 1;
    char *slash = strrchr( core, '/' );
    if (!slash || (size_t)(slash - core) + sizeof "/inversa-core" > sizeof core) return 1;
    strcpy( slash, "/inversa-core" );
    if (getpagesize() != 0x1000 && !getenv( "INVERSA_NO_4K" ) && entitled())
    {
        posix_spawnattr_t attr;
        posix_spawnattr_init( &attr );
        posix_spawnattr_setflags( &attr, POSIX_SPAWN_SETEXEC );
        posix_spawnattr_set_4k_page_size_np( &attr );
        posix_spawn( NULL, core, NULL, &attr, argv, environ );
    }
    execve( core, argv, environ );   // no entitlement (or the 4K spawn failed): the core says why
    perror( core );
    return 1;
}
