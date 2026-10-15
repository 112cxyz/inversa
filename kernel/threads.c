// Threads and signals as Linux sees them. Thread ids are Mach thread ports: 32-bit and unique
// while the thread lives, like a Linux tid.
#include "kernel.h"
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <sys/ucontext.h>

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

// The raw rt_sigaction ABI used by FEX installs a Linux-arm64 handler. Darwin
// passes a different siginfo and ucontext, so build the Linux frame it expects
// and copy any register changes back before returning to the interrupted code.
struct linux_action { uint64_t handler, flags, restorer, mask; };
static struct linux_action actions[65];

struct linux_stack { uint64_t sp; int32_t flags, pad; uint64_t size; };
struct linux_mcontext {
    uint64_t fault_address, regs[31], sp, pc, pstate;
    uint8_t reserved[4096] __attribute__((aligned(16)));
};
struct linux_ucontext {
    uint64_t flags, link;
    struct linux_stack stack;
    uint64_t sigmask[16];
    struct linux_mcontext mcontext;
};
_Static_assert( offsetof(struct linux_ucontext, mcontext) == 176, "Linux ucontext offset" );

struct linux_siginfo {
    int32_t signo, error, code, pad;
    uint64_t data[14];
};
_Static_assert( sizeof(struct linux_siginfo) == 128, "Linux siginfo size" );

struct fpsimd_record {
    uint32_t magic, size, fpsr, fpcr;
    __uint128_t v[32];
};
_Static_assert( sizeof(struct fpsimd_record) == 528, "Linux fpsimd context size" );

int mvr_jit_fault( void *addr, int is_write, int is_exec );

static void deliver( int darwin_sig, siginfo_t *info, void *raw )
{
    ucontext_t *host = raw;
    _STRUCT_ARM_THREAD_STATE64 *ss = &host->uc_mcontext->__ss;
    // a fault on MAP_JIT memory is first this thread's write/execute switch, which the guest
    // never sees (its handler would return to the same fault forever)
    if (darwin_sig == SIGSEGV || darwin_sig == SIGBUS)
    {
        uint32_t esr = host->uc_mcontext->__es.__esr, ec = esr >> 26;
        int is_exec = ec == 0x20 || ec == 0x21;
        int is_write = (ec == 0x24 || ec == 0x25) && (esr & (1u << 6));
        if (mvr_jit_fault( is_exec ? (void *)ss->__pc : info->si_addr, is_write, is_exec )) return;
    }
    int sig = inv_sig_to_linux( darwin_sig );
    if (!sig || actions[sig].handler <= 1) return;
    struct linux_ucontext linux = {0};
    struct linux_siginfo li = {0};
    // Darwin's uc_stack.ss_sp describes the interrupted stack position here,
    // while Linux's ucontext carries the registered alternate stack base.
    stack_t alt = {0};
    if (sigaltstack( NULL, &alt )) alt = host->uc_stack;
    linux.stack.sp = (uintptr_t)alt.ss_sp;
    linux.stack.size = alt.ss_size;
    linux.stack.flags = (alt.ss_flags & SS_ONSTACK ? 1 : 0) |
                        (alt.ss_flags & SS_DISABLE ? 2 : 0);
    for (int i = 1; i <= 64; i++)
    {
        int d = inv_sig_to_darwin( i );
        if (d && sigismember( &host->uc_sigmask, d ) == 1) linux.sigmask[0] |= 1ull << (i - 1);
    }
    linux.mcontext.fault_address = (uintptr_t)info->si_addr;
    memcpy( linux.mcontext.regs, ss->__x, 29 * sizeof(uint64_t) );
    linux.mcontext.regs[29] = ss->__fp;
    linux.mcontext.regs[30] = ss->__lr;
    linux.mcontext.sp = ss->__sp;
    linux.mcontext.pc = ss->__pc;
    linux.mcontext.pstate = ss->__cpsr;
    struct fpsimd_record *fp = (void *)linux.mcontext.reserved;
    fp->magic = 0x46508001; fp->size = sizeof *fp;
    fp->fpsr = host->uc_mcontext->__ns.__fpsr;
    fp->fpcr = host->uc_mcontext->__ns.__fpcr;
    memcpy( fp->v, host->uc_mcontext->__ns.__v, sizeof fp->v );
    uint32_t *esr = (uint32_t *)(linux.mcontext.reserved + sizeof *fp);
    esr[0] = 0x45535201; esr[1] = 16;
    *(uint64_t *)(esr + 2) = host->uc_mcontext->__es.__esr;
    li.signo = sig; li.error = info->si_errno; li.code = info->si_code;
    if (sig == 17) // SIGCHLD: pid, uid, status, utime, stime
    {
        int32_t *words = (int32_t *)li.data;
        words[0] = info->si_pid; words[1] = info->si_uid; words[2] = info->si_status;
        li.data[2] = 0; li.data[3] = 0;
    }
    else li.data[0] = (uintptr_t)info->si_addr;
    ((void (*)(int, void *, void *))(uintptr_t)actions[sig].handler)( sig, &li, &linux );
    memcpy( ss->__x, linux.mcontext.regs, 29 * sizeof(uint64_t) );
    ss->__fp = linux.mcontext.regs[29]; ss->__lr = linux.mcontext.regs[30];
    ss->__sp = linux.mcontext.sp; ss->__pc = linux.mcontext.pc;
    ss->__cpsr = (uint32_t)linux.mcontext.pstate;
    fp = (void *)linux.mcontext.reserved;
    if (fp->magic == 0x46508001 && fp->size >= sizeof *fp)
    {
        host->uc_mcontext->__ns.__fpsr = fp->fpsr;
        host->uc_mcontext->__ns.__fpcr = fp->fpcr;
        memcpy( host->uc_mcontext->__ns.__v, fp->v, sizeof fp->v );
    }
    sigemptyset( &host->uc_sigmask );
    for (int i = 1; i <= 64; i++)
    {
        int d = inv_sig_to_darwin( i );
        if (d && (linux.sigmask[0] & (1ull << (i - 1)))) sigaddset( &host->uc_sigmask, d );
    }
}

void inv_signal_installed( int sig, const void *action )
{
    int darwin = inv_sig_to_darwin( sig );
    if (!darwin) return;
    actions[sig] = *(const struct linux_action *)action;
    struct sigaction sa = {0};
    sigemptyset( &sa.sa_mask );
    for (int i = 1; i <= 64; i++)
    {
        int d = inv_sig_to_darwin( i );
        if (d && (actions[sig].mask & (1ull << (i - 1)))) sigaddset( &sa.sa_mask, d );
    }
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    if (actions[sig].flags & 0x10000000) sa.sa_flags |= SA_RESTART;
    if (actions[sig].flags & 0x40000000) sa.sa_flags |= SA_NODEFER;
    sa.sa_sigaction = deliver;
    if (actions[sig].handler <= 1)
    {
        sa.sa_flags = 0;
        sa.sa_handler = actions[sig].handler ? SIG_IGN : SIG_DFL;
    }
    sigaction( darwin, &sa, NULL );
}
