// Threads and signals as Linux sees them. Thread ids are Mach thread ports: 32-bit and unique
// while the thread lives, like a Linux tid.
#include "kernel.h"
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

long inv_tkill( long tid, long sig )
{
    pthread_t t = pthread_from_mach_thread_np( (mach_port_t)tid );
    if (!t) { errno = ESRCH; return -1; }
    int d = inv_sig_to_darwin( (int)sig );
    if (sig && !d) { errno = EINVAL; return -1; }
    int r = pthread_kill( t, d );
    if (r) { errno = r; return -1; }
    return 0;
}

void inv_thread_exit( int code ) { pthread_exit( NULL ); }

// delivery to guest handlers comes with signals.c; until then a guest handler is noted only
void inv_signal_installed( int sig, const void *action )
{
    if (getenv( "INVERSA_STRACE" )) fprintf( stderr, "inversa: guest handler for signal %d (not delivered yet)\n", sig );
}
