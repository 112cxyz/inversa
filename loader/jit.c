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

void mvr_jit_after_fork_child(void)
{
    // The hardware's per-thread JIT protection can reset across fork while this TLS
    // value is copied from the parent. The child resumes translated code immediately.
    pthread_jit_write_protect_np(1);
    writing = 0;
}

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

/* both ways at once for code that's about to write JIT memory with faults blocked (a fault
 * handler), and for going back to running it after */
void mvr_jit_mode( int write )
{
    pthread_jit_write_protect_np( write ? 0 : 1 );
    writing = write;
}

/* from the fault handler: 1 if the fault was the W^X switch and the access can retry. JIT
 * memory reads in either mode, so any data fault on it wants writing (atomics come in with WnR
 * clear) and an instruction fault wants executing. The switch is set, not toggled from what
 * this thread last chose: other code flips it (the kernel layer's rewriting) and signal delivery
 * can bring a handler in executing, so "already writing" was sometimes wrong and FEX's handler
 * spun on its own code-buffer lock. The same fault straight after a switch isn't the switch. */
int mvr_jit_fault( void *addr, int is_write, int is_exec )
{
    static __thread uintptr_t last_addr;
    static __thread int last_exec, repeats;
    (void)is_write;
    if (!mvr_jit_contains( addr )) return 0;
    if ((uintptr_t)addr == last_addr && is_exec == last_exec)
    {
        if (++repeats > 2) { last_addr = 0; repeats = 0; return 0; }
    }
    else repeats = 0;
    last_addr = (uintptr_t)addr; last_exec = is_exec;
    pthread_jit_write_protect_np( is_exec ? 1 : 0 );
    writing = !is_exec;
    return 1;
}
