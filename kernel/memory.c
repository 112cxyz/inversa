// Guest memory, Linux style, on Darwin. Below 4 GB the glibc layer has reserved the whole range
// (it sits over PAGEZERO), so "free" there means not handed out, tracked here page by page: every
// path (MAP_FIXED, MAP_FIXED_NOREPLACE, a hint, munmap) keeps the map current, which FEX's
// page-exact guest mappings need. Above 4 GB Darwin's own VM map is the truth, and
// MAP_FIXED_NOREPLACE claims a range atomically with a fixed mach_vm_allocate.
//
// Executable memory: FEX's code buffers (anonymous PROT_EXEC) must run, which on Apple silicon
// means MAP_JIT, made by the glibc layer's JIT path. Guest x86 code is only data to FEX, so in
// x86-64 mode PROT_EXEC on anything else is dropped (Darwin won't map unsigned files executable).
#include "kernel.h"
#include <errno.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

void *mvr_glibc_ext_lookup( const char *name );
int mvr_jit_mprotect( void *addr, size_t len, int prot );
int mvr_jit_contains( void *addr );
void mvr_jit_unmap( void *addr, size_t len );

int inv_x86_mode;
extern int inv_arm64_mode;
void *inv_map_code( void *addr, size_t len, int fixed, int fd, off_t off );

enum { L_SHARED = 0x01, L_FIXED = 0x10, L_ANON = 0x20, L_NOREPLACE = 0x100000 };
#define PAGE 0x1000ull
#define LOW_START 0x10000ull     // as the glibc layer reserves it
#define LOW_END 0x100000000ull

static uint8_t low_pages[(LOW_END - LOW_START) / PAGE / 8];
static pthread_mutex_t low_mu = PTHREAD_MUTEX_INITIALIZER;
static int low_reserved = -1;

typedef void *(*mmap_fn)( void *, size_t, long, long, long, long );
static mmap_fn base_mmap;

static int is_low( uintptr_t a, size_t len ) { return low_reserved > 0 && a >= LOW_START && a + len <= LOW_END && a + len > a; }

static int low_any( uintptr_t a, size_t len )
{
    for (uintptr_t p = a; p < a + len; p += PAGE)
    {
        size_t i = (p - LOW_START) / PAGE;
        if (low_pages[i / 8] & (1 << (i % 8))) return 1;
    }
    return 0;
}

static void low_mark( uintptr_t a, size_t len, int used )
{
    for (uintptr_t p = a; p < a + len; p += PAGE)
    {
        size_t i = (p - LOW_START) / PAGE;
        if (used) low_pages[i / 8] |= 1 << (i % 8); else low_pages[i / 8] &= ~(1 << (i % 8));
    }
}

static void init(void)
{
    if (low_reserved >= 0) return;
    base_mmap = (mmap_fn)mvr_glibc_ext_lookup( "mmap" );
    // the glibc layer reserved the low range if the entitlement let it: is 64 KB taken?
    mach_vm_address_t a = LOW_START; mach_vm_size_t sz; mach_port_t obj;
    vm_region_basic_info_data_64_t info; mach_msg_type_number_t n = VM_REGION_BASIC_INFO_COUNT_64;
    low_reserved = mach_vm_region( mach_task_self(), &a, &sz, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &n, &obj ) == KERN_SUCCESS
                   && a <= LOW_START;
    if (getenv( "INVERSA_STRACE" )) fprintf( stderr, "[inv] low memory %s (first region %#llx+%#llx)\n", low_reserved ? "reserved" : "not reserved", a, sz );
}

// the mapping itself, with the executable rules above
static void *map( void *addr, size_t len, long prot, long flags, long fd, long off )
{
    if (inv_arm64_mode && fd >= 0 && (prot & PROT_EXEC))
        return inv_map_code( addr, len, (flags & L_FIXED) != 0, (int)fd, (off_t)off );
    if (!(flags & L_ANON) || (inv_x86_mode && !(prot & PROT_EXEC)))
        return base_mmap( addr, len, (flags & L_ANON) ? prot : prot & ~PROT_EXEC, flags, fd, off );
    if (!(prot & PROT_EXEC)) return base_mmap( addr, len, prot, flags, fd, off );
    void *p = base_mmap( addr, len, prot & ~PROT_EXEC, flags, fd, off );
    if (p != MAP_FAILED && mvr_jit_mprotect( p, len, (int)prot ))
    {
        munmap( p, len );
        errno = ENOMEM;
        return MAP_FAILED;
    }
    return p;
}

static void *map_low( uintptr_t a, size_t len, long prot, long flags, long fd, long off )
{
    pthread_mutex_lock( &low_mu );
    void *p;
    if (!(flags & L_FIXED) && low_any( a, len )) { errno = EEXIST; p = MAP_FAILED; }
    else
    {
        p = map( (void *)a, len, prot, (flags & ~L_NOREPLACE) | L_FIXED, fd, off );
        if (p != MAP_FAILED) low_mark( a, len, 1 );
    }
    pthread_mutex_unlock( &low_mu );
    return p;
}

static void *map_high_noreplace( void *addr, size_t len, long prot, long flags, long fd, long off )
{
    mach_vm_address_t a = (mach_vm_address_t)addr;
    kern_return_t kr = mach_vm_allocate( mach_task_self(), &a, len, VM_FLAGS_FIXED );
    if (kr != KERN_SUCCESS)
    {
        errno = kr == KERN_NO_SPACE ? EEXIST : ENOMEM;
        return MAP_FAILED;
    }
    void *p = map( addr, len, prot, (flags & ~L_NOREPLACE) | L_FIXED, fd, off );
    if (p == MAP_FAILED) mach_vm_deallocate( mach_task_self(), a, len );
    return p;
}

static void *do_mmap( void *addr, size_t len, long prot, long flags, long fd, long off )
{
    init();
    uintptr_t a = (uintptr_t)addr;
    size_t plen = (len + PAGE - 1) & ~(PAGE - 1);
    if (!len) { errno = EINVAL; return MAP_FAILED; }
    if (a && is_low( a & ~(PAGE - 1), plen ))
    {
        if (flags & (L_FIXED | L_NOREPLACE)) return map_low( a, plen, prot, flags, fd, off );
        void *p = map_low( a & ~(PAGE - 1), plen, prot, flags, fd, off );   // a hint: there if free
        if (p != MAP_FAILED) return p;
        return map( NULL, len, prot, flags & ~L_FIXED, fd, off );
    }
    if (flags & L_NOREPLACE) return map_high_noreplace( addr, len, prot, flags, fd, off );
    return map( addr, len, prot, flags, fd, off );
}

void *inv_mmap( void *addr, size_t len, long prot, long flags, long fd, long off )
{
    static int trace = -1;
    if (trace < 0) trace = getenv( "INVERSA_STRACE" ) && *getenv( "INVERSA_STRACE" ) != '0';
    void *p = do_mmap( addr, len, prot, flags, fd, off );
    if (trace)
        fprintf( stderr, "[inv] mmap(%p, %#zx, %ld, %#lx, %ld, %#lx) = %p%s\n", addr, len, prot, flags, fd, off, p,
                 p == MAP_FAILED ? " failed" : "" );
    return p;
}

long inv_munmap( void *addr, size_t len )
{
    init();
    uintptr_t a = (uintptr_t)addr;
    size_t plen = (len + PAGE - 1) & ~(PAGE - 1);
    if ((a & (PAGE - 1)) || !len) { errno = EINVAL; return -1; }
    mvr_jit_unmap( addr, plen );
    if (is_low( a, plen ))
    {
        pthread_mutex_lock( &low_mu );
        // back to the reservation, not a hole Darwin could hand out
        mmap( addr, plen, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED | MAP_NORESERVE, -1, 0 );
        low_mark( a, plen, 0 );
        pthread_mutex_unlock( &low_mu );
        return 0;
    }
    return munmap( addr, plen );
}

long inv_mprotect( void *addr, size_t len, long prot )
{
    typedef int (*mprotect_fn)( void *, size_t, long );
    static mprotect_fn base;
    if (!base) base = (mprotect_fn)mvr_glibc_ext_lookup( "mprotect" );
    if (inv_x86_mode && !mvr_jit_contains( addr )) return mprotect( addr, len, (int)(prot & ~PROT_EXEC) );
    return base( addr, len, prot );
}
