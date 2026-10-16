// Whether this process has the cross-architecture entitlement. Without it, asking for 4K pages
// gets the process killed, so both the launcher and the core check first.
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>

static int has_xarch_entitlement(void)
{
    SecTaskRef task = SecTaskCreateFromSelf( NULL );
    if (!task) return 0;
    CFTypeRef v = SecTaskCopyValueForEntitlement( task, CFSTR( "com.apple.developer.cross-architecture-support" ), NULL );
    int on = v && CFGetTypeID( v ) == CFBooleanGetTypeID() && CFBooleanGetValue( (CFBooleanRef)v );
    if (v) CFRelease( v );
    CFRelease( task );
    return on;
}
