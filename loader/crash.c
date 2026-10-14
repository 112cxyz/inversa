// Fatal-signal report for guest crashes: registers and a frame-pointer walk, each
// address named as library+offset (lldb can't see guest images).
#include "klepton.h"
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ucontext.h>
#include <unistd.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>

const char *kl_addr_image( const void *addr, size_t *offset );

static void name( char *out, size_t n, uint64_t a )
{
    size_t off = 0;
    const char *img = a ? kl_addr_image( (void *)a, &off ) : NULL;
    Dl_info di;
    if (img) snprintf( out, n, "%s+%#zx", strrchr( img, '/' ) ? strrchr( img, '/' ) + 1 : img, off );
    else if (a && dladdr( (void *)a, &di ) && di.dli_sname) snprintf( out, n, "%s+%#lx (%s)", di.dli_sname, (unsigned long)(a - (uint64_t)di.dli_saddr), strrchr( di.dli_fname, '/' ) + 1 );
    else snprintf( out, n, "%#llx", (unsigned long long)a );
}

int mvr_jit_fault( void *addr, int is_write, int is_exec );

static void handler( int sig, siginfo_t *si, void *ctx )
{
    ucontext_t *uc = ctx;
    struct __darwin_arm_thread_state64 *ss = &uc->uc_mcontext->__ss;
    uint32_t esr = uc->uc_mcontext->__es.__esr;
    unsigned ec = esr >> 26;
    int is_exec = ec == 0x20 || ec == 0x21;
    int is_write = (ec == 0x24 || ec == 0x25) && (esr & (1u << 6));
    if ((sig == SIGBUS || sig == SIGSEGV) && mvr_jit_fault( is_exec ? (void *)ss->__pc : si->si_addr, is_write, is_exec )) return;
    char b[256];
    fprintf( stderr, "\n[mvr] fatal signal %d (%s) at address %p\n", sig, strsignal( sig ), si->si_addr );
    name( b, sizeof b, ss->__pc ); fprintf( stderr, "  pc  %s\n", b );
    name( b, sizeof b, ss->__lr ); fprintf( stderr, "  lr  %s\n", b );
    for (int i = 0; i < (getenv( "MVR_CRASH_REGS" ) ? 29 : 8); i++)
        fprintf( stderr, "%s x%d=%#llx", i % 8 ? "" : "\n ", i, (unsigned long long)ss->__x[i] );
    const char *peek = getenv( "MVR_CRASH_PEEK" );   /* "reg,off": print the 8 qwords there */
    if (peek)
    {
        char *e; int r = strtol( peek, &e, 10 ); long off = strtol( e + 1, NULL, 0 );
        uint64_t *q = (uint64_t *)(ss->__x[r] + off);
        for (int i = 0; i < 8; i++)
        {
            char w[256]; name( w, sizeof w, q[i] );
            fprintf( stderr, "\n  [x%d+%#lx] = %s", r, off + i * 8, w );
        }
    }
    {
        mach_vm_address_t ra = (mach_vm_address_t)si->si_addr; mach_vm_size_t rs = 0;
        vm_region_basic_info_data_64_t info; mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64; mach_port_t obj;
        if (!mach_vm_region( mach_task_self(), &ra, &rs, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &cnt, &obj ))
            fprintf( stderr, "\n  fault region %#llx-%#llx prot %d/%d %s", (unsigned long long)ra, (unsigned long long)(ra + rs),
                     info.protection, info.max_protection, ra > (mach_vm_address_t)si->si_addr ? "(address unmapped; next region shown)" : "" );
    }
    if (getenv( "MVR_CRASH_STACK" ))   /* code addresses on the stack: return addresses when frames are lost */
    {
        fprintf( stderr, "\n  sp %#llx, code addresses on the stack:", (unsigned long long)ss->__sp );
        uint64_t *sp = (uint64_t *)ss->__sp;
        for (int i = 0; i < 512; i++)
        {
            size_t off; const char *img = kl_addr_image( (void *)sp[i], &off );
            Dl_info di;
            if (img) fprintf( stderr, "\n    [sp+%#x] %s+%#zx", i * 8, img, off );
            else if (sp[i] > 0x180000000ull && sp[i] < 0x300000000ull && dladdr( (void *)sp[i], &di ) && di.dli_sname)
                fprintf( stderr, "\n    [sp+%#x] %s+%#lx", i * 8, di.dli_sname, (long)(sp[i] - (uint64_t)di.dli_saddr) );
        }
    }
    fprintf( stderr, "\n  backtrace:\n" );
    uint64_t *fp = (uint64_t *)ss->__fp;
    for (int i = 0; i < 32 && fp && ((uintptr_t)fp & 7) == 0; i++)
    {
        uint64_t ret = fp[1];
        if (!ret) break;
        name( b, sizeof b, ret ); fprintf( stderr, "    #%-2d %s\n", i, b );
        fp = (uint64_t *)fp[0];
    }
    fflush( NULL );
    _exit( 128 + sig );
}

/* SIGUSR1: write the guest image map, to symbolise `sample` output of a live process */
static void print_image( const char *name, void *base, size_t span, void *ctx )
{
    fprintf( ctx, "%p %p %s\n", base, (char *)base + span, name );
}

static void dump_images( int sig )
{
    char path[64];
    snprintf( path, sizeof path, "/tmp/mvr-images-%d.txt", getpid() );
    FILE *f = fopen( path, "w" );
    if (!f) return;
    kl_each_image( print_image, f );
    fclose( f );
}

void mvr_crash_init(void)
{
    signal( SIGUSR1, dump_images );
    struct sigaction sa = { 0 };
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    static char altstack[64 * 1024];
    stack_t st = { .ss_sp = altstack, .ss_size = sizeof altstack };
    sigaltstack( &st, NULL );
    sigaction( SIGSEGV, &sa, NULL );
    sigaction( SIGBUS, &sa, NULL );
    sigaction( SIGILL, &sa, NULL );
}
