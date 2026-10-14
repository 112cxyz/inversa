// Guest JITs (LuaJIT and friends) write code into anonymous memory and mprotect it
// executable. Apple silicon only runs unsigned code from MAP_JIT memory, whose
// protection can't change and whose write/execute switch is per thread. So a range
// made executable is remapped in place as MAP_JIT (contents preserved; this needs the
// 4K-page, cross-architecture-entitled process), later mprotects on it are accepted,
// and faults flip the faulting thread between writing and executing it.
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static struct { uintptr_t start, end; } ranges[1024];
static int nranges;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static __thread int writing;     /* this thread's view of MAP_JIT memory */

void mvr_jit_register( void *addr, size_t len )
{
    pthread_mutex_lock( &lock );
    if (nranges < 1024)
    {
        ranges[nranges].start = (uintptr_t)addr;
        ranges[nranges].end = (uintptr_t)addr + len;
        nranges++;
    }
    pthread_mutex_unlock( &lock );
}

static int find( uintptr_t a, size_t len )
{
    for (int i = 0; i < nranges; i++)
        if (a >= ranges[i].start && a + len <= ranges[i].end) return i;
    return -1;
}

int mvr_jit_contains( void *addr ) { return find( (uintptr_t)addr, 1 ) >= 0; }

/* returns 0 if handled (made JIT or already JIT), -1 to fall through to a plain mprotect */
int mvr_jit_mprotect( void *addr, size_t len, int prot )
{
    uintptr_t a = (uintptr_t)addr;
    pthread_mutex_lock( &lock );
    int i = find( a, len );
    pthread_mutex_unlock( &lock );
    if (i >= 0) return 0;                   /* already JIT: protection stays RWX */
    if (!(prot & PROT_EXEC)) return -1;

    void *copy = malloc( len );
    if (!copy) return -1;
    /* the range may still be PROT_NONE (a reservation the guest is committing as code) */
    if (mprotect( addr, len, PROT_READ | PROT_WRITE )) { free( copy ); return -1; }
    memcpy( copy, addr, len );
    void *p = mmap( addr, len, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_FIXED | MAP_JIT, -1, 0 );
    if (p == MAP_FAILED) { free( copy ); return -1; }
    pthread_jit_write_protect_np( 0 );
    memcpy( addr, copy, len );
    pthread_jit_write_protect_np( 1 );
    writing = 0;
    __builtin___clear_cache( addr, (char *)addr + len );
    free( copy );
    pthread_mutex_lock( &lock );
    if (nranges < 1024) { ranges[nranges].start = a; ranges[nranges].end = a + len; nranges++; }
    pthread_mutex_unlock( &lock );
    return 0;
}

void mvr_jit_unmap( void *addr, size_t len )
{
    uintptr_t a = (uintptr_t)addr;
    pthread_mutex_lock( &lock );
    for (int i = 0; i < nranges; i++)
        if (ranges[i].start >= a && ranges[i].end <= a + len) ranges[i--] = ranges[--nranges];
    pthread_mutex_unlock( &lock );
}

/* from the fault handler: 1 if the fault was the W^X switch and the access can retry */
int mvr_jit_fault( void *addr, int is_write, int is_exec )
{
    if (!mvr_jit_contains( addr )) return 0;
    if (is_write && !writing) { pthread_jit_write_protect_np( 0 ); writing = 1; return 1; }
    if (is_exec && writing) { pthread_jit_write_protect_np( 1 ); writing = 0; return 1; }
    if (!is_write && !is_exec) return 0;
    return 0;
}
