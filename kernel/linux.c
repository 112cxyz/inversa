// ARM64 Linux userspace as it is: the program's own ld.so and glibc, over Inversa's kernel.
//
// Darwin can't run that code unchanged: it only executes unsigned pages mapped MAP_JIT, a
// `svc #0` would be taken as a Darwin syscall, TPIDR_EL0 (glibc's thread pointer) is
// overwritten by XNU with the CPU number, and EL0 reads of CTR_EL0 / MIDR_EL1 / the ID
// registers trap. So executable mappings are loaded into MAP_JIT memory and those
// instructions rewritten to `brk`s that land in trap() below:
//   brk #0x5100        svc #0               -> inv_syscall
//   brk #0x52<Rt>      mrs Rt, tpidr_el0    -> this thread's guest TP
//   brk #0x53<Rt>      msr tpidr_el0, Rt
//   brk #0x54<Rt>      mrs Rt, ctr_el0
//   brk #0x6<CRm,op2,Rt>  mrs Rt, <ID register> (op0=3 op1=0 CRn=0)
#include "kernel.h"
#include <errno.h>
#include <fcntl.h>
#include <libkern/OSCacheControl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/ucontext.h>
#include <unistd.h>

int mvr_errno_to_linux( int e );
void mvr_jit_register( void *addr, size_t len );
extern void mvr_enter( void *entry, void *sp ) __attribute__((noreturn));

int inv_arm64_mode;
static char root[PATH_MAX];
static __thread uint64_t guest_tp;
static uint64_t id_values[16][8];
static uintptr_t heap_start, heap_end, heap_break;

void inv_cpu_feature_registers( char *out, size_t cap );

static void init_ids(void)
{
    char s[512];
    unsigned long long isar0, isar1, isar2, pfr0, pfr1, mmfr1, mmfr2, dczid;
    inv_cpu_feature_registers( s, sizeof s );
    if (sscanf( s, "isar0=%llx,isar1=%llx,isar2=%llx,pfr0=%llx,pfr1=%llx,mmfr1=%llx,mmfr2=%llx,dczid=%llx",
                &isar0, &isar1, &isar2, &pfr0, &pfr1, &mmfr1, &mmfr2, &dczid ) != 8) abort();
    id_values[0][0] = 0x610f0000; // MIDR_EL1, matching the FEX feature set
    id_values[4][0] = pfr0; id_values[4][1] = pfr1;
    id_values[6][0] = isar0; id_values[6][1] = isar1; id_values[6][2] = isar2;
    id_values[7][1] = mmfr1; id_values[7][2] = mmfr2;
}

void inv_cpu_id_register( unsigned crm, unsigned op2, uint64_t *out )
{
    *out = id_values[crm][op2];
}

long inv_arm64_brk( uintptr_t requested )
{
    if (!heap_start)
    {
        void *p = mmap( NULL, 64 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0 );
        if (p == MAP_FAILED) return 0;
        heap_start = heap_break = (uintptr_t)p;
        heap_end = heap_start + (64 << 20);
    }
    if (requested >= heap_start && requested <= heap_end) heap_break = requested;
    return (long)heap_break;
}

/* ---- rewriting ---------------------------------------------------------------- */
static uint32_t brk_insn( uint32_t imm ) { return 0xd4200000u | (imm & 0xffff) << 5; }

static size_t rewrite( uint32_t *code, size_t words )
{
    size_t n = 0;
    for (size_t i = 0; i < words; i++)
    {
        uint32_t w = code[i], rt = w & 31;
        if ((w & 0xffe0001fu) == 0xd4000001u) code[i] = brk_insn( 0x5100 );          // svc #imm16
        else if ((w & ~31u) == 0xd53bd040u) code[i] = brk_insn( 0x5200 | rt );      // mrs tpidr_el0
        else if ((w & ~31u) == 0xd51bd040u) code[i] = brk_insn( 0x5300 | rt );      // msr tpidr_el0
        else if ((w & ~31u) == 0xd53b0020u) code[i] = brk_insn( 0x5400 | rt );      // mrs ctr_el0
        else if ((w & 0xfffff000u) == 0xd5380000u) code[i] = brk_insn( 0x6000 | ((w >> 5) & 0x7f) << 5 | rt );  // ID space
        else continue;
        n++;
    }
    return n;
}

// A file's code as MAP_JIT memory, rewritten. The requested length includes the
// zero-filled tail of the final file page, as with a normal file mapping.
void *inv_map_code( void *addr, size_t len, int fixed, int fd, off_t off )
{
    void *p = mmap( addr, len, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT | (fixed ? MAP_FIXED : 0), -1, 0 );
    if (p == MAP_FAILED) return p;
    pthread_jit_write_protect_np( 0 );
    size_t got = 0;
    while (got < len)
    {
        ssize_t r = pread( fd, (char *)p + got, len - got, off + (off_t)got );
        if (r > 0) { got += (size_t)r; continue; }
        if (!r) break;
        if (errno == EINTR) continue;
        int e = errno;
        pthread_jit_write_protect_np( 1 );
        munmap( p, len );
        errno = e;
        return MAP_FAILED;
    }
    size_t n = rewrite( p, got / 4 );
    pthread_jit_write_protect_np( 1 );
    sys_icache_invalidate( p, len );
    mvr_jit_register( p, len );
    if (getenv( "INVERSA_STRACE" )) fprintf( stderr, "[inv] code %p+%#zx from fd %d: %zu instructions rewritten\n", p, len, fd, n );
    return p;
}

/* ---- paths: absolute ones look in the guest root first --------------------------- */
const char *inv_guest_path( const char *p, char *buf )
{
    if (!root[0] || !p || p[0] != '/' || !strncmp( p, "/proc", 5 ) || !strncmp( p, "/dev", 4 ) || !strncmp( p, "/tmp", 4 ))
        return p;
    struct stat st;
    snprintf( buf, PATH_MAX, "%s%s", root, p );
    return lstat( buf, &st ) ? p : buf;
}

/* ---- traps ------------------------------------------------------------------------ */
static uint64_t *reg( _STRUCT_ARM_THREAD_STATE64 *ss, unsigned r )
{
    static uint64_t zero;
    if (r < 29) return &ss->__x[r];
    if (r == 29) return &ss->__fp;
    if (r == 30) return &ss->__lr;
    zero = 0;
    return &zero;   // xzr
}

static void trap( int sig, siginfo_t *si, void *ctx )
{
    ucontext_t *uc = ctx;
    _STRUCT_ARM_THREAD_STATE64 *ss = &uc->uc_mcontext->__ss;
    uint32_t esr = uc->uc_mcontext->__es.__esr, imm = esr & 0xffff;
    if (esr >> 26 != 0x3c || imm < 0x5100 || imm > 0x6fff)
    {
        fprintf( stderr, "inversa: guest brk #%#x at %#llx\n", imm, (unsigned long long)ss->__pc );
        _exit( 128 + SIGTRAP );
    }
    unsigned rt = imm & 31;
    switch (imm >> 8)
    {
    case 0x51: {
        long r = inv_syscall( (long)ss->__x[8], (long)ss->__x[0], (long)ss->__x[1], (long)ss->__x[2],
                              (long)ss->__x[3], (long)ss->__x[4], (long)ss->__x[5] );
        ss->__x[0] = (uint64_t)(r == -1 ? -(long)mvr_errno_to_linux( errno ) : r);
        break;
    }
    case 0x52: *reg( ss, rt ) = guest_tp; break;
    case 0x53: guest_tp = *reg( ss, rt ); break;
    case 0x54: *reg( ss, rt ) = 0x8555c004; break;   // 128-byte D lines, 64-byte I lines
    default:
        if (imm >> 12 == 6) inv_cpu_id_register( (imm >> 8) & 15, (imm >> 5) & 7, reg( ss, rt ) );
    }
    ss->__pc += 4;
}

/* ---- exec ------------------------------------------------------------------------- */
typedef struct { uint8_t ident[16]; uint16_t type, machine; uint32_t version; uint64_t entry, phoff, shoff;
                 uint32_t flags; uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx; } ehdr;
typedef struct { uint32_t type, flags; uint64_t offset, vaddr, paddr, filesz, memsz, align; } phdr;
enum { PT_LOAD = 1, PT_INTERP = 3, PT_PHDR = 6, PF_X = 1, PF_W = 2, PF_R = 4 };

void *inv_mmap( void *addr, size_t len, long prot, long flags, long fd, long off );

typedef struct { uint64_t base, entry, phdr; uint16_t phnum; char interp[PATH_MAX]; } image;

static int load( const char *path, image *im )
{
    int fd = open( path, O_RDONLY );
    if (fd < 0) { fprintf( stderr, "inversa: %s: %s\n", path, strerror( errno ) ); return -1; }
    ehdr eh; phdr ph[64];
    if (pread( fd, &eh, sizeof eh, 0 ) != sizeof eh || memcmp( eh.ident, "\177ELF\2\1", 6 ) || eh.machine != 183 ||
        eh.phnum > 64 || pread( fd, ph, eh.phnum * sizeof *ph, (off_t)eh.phoff ) != (ssize_t)(eh.phnum * sizeof *ph))
    {
        fprintf( stderr, "inversa: %s: not an ARM64 ELF image\n", path );
        close( fd ); return -1;
    }
    uint64_t lo = ~0ull, hi = 0;
    im->interp[0] = 0; im->phdr = 0;
    for (int i = 0; i < eh.phnum; i++)
    {
        if (ph[i].type == PT_LOAD)
        {
            if (ph[i].vaddr < lo) lo = ph[i].vaddr & ~0xfffull;
            if (ph[i].vaddr + ph[i].memsz > hi) hi = ph[i].vaddr + ph[i].memsz;
        }
        if (ph[i].type == PT_INTERP && ph[i].filesz < PATH_MAX)
        {
            pread( fd, im->interp, ph[i].filesz, (off_t)ph[i].offset );
            im->interp[ph[i].filesz] = 0;
        }
    }
    hi = (hi + 0xfff) & ~0xfffull;
    // ET_DYN goes wherever there's room: reserve the span, then map each segment over it
    uint64_t base = 0;
    if (eh.type == 3)
    {
        void *span = inv_mmap( NULL, hi - lo, PROT_NONE, 0x22, -1, 0 );   // Linux MAP_PRIVATE|MAP_ANONYMOUS
        if (span == MAP_FAILED) { close( fd ); return -1; }
        base = (uint64_t)span - lo;
    }
    for (int i = 0; i < eh.phnum; i++)
    {
        if (ph[i].type != PT_LOAD) continue;
        uint64_t va = base + ph[i].vaddr, start = va & ~0xfffull, delta = va - start;
        uint64_t fileend = va + ph[i].filesz, end = (va + ph[i].memsz + 0xfff) & ~0xfffull;
        int prot = (ph[i].flags & PF_R ? PROT_READ : 0) | (ph[i].flags & PF_W ? PROT_WRITE : 0) | (ph[i].flags & PF_X ? PROT_EXEC : 0);
        if (ph[i].filesz &&
            inv_mmap( (void *)start, ph[i].filesz + delta, prot | PROT_WRITE, 0x12, fd, (long)(ph[i].offset - delta) ) == MAP_FAILED)
        {
            fprintf( stderr, "inversa: %s: can't map segment %d\n", path, i );
            close( fd ); return -1;
        }
        // .bss: the rest of the last file page, then whole anonymous pages
        uint64_t pageend = (fileend + 0xfff) & ~0xfffull;
        if (ph[i].memsz > ph[i].filesz)
        {
            if (fileend < pageend) memset( (void *)fileend, 0, pageend - fileend );
            if (end > pageend) inv_mmap( (void *)pageend, end - pageend, prot, 0x32, -1, 0 );   // FIXED|PRIVATE|ANON
        }
        if (!(prot & PROT_WRITE) && !(prot & PROT_EXEC)) mprotect( (void *)start, end - start, prot );
        if (ph[i].offset <= eh.phoff && eh.phoff < ph[i].offset + ph[i].filesz)
            im->phdr = va + (eh.phoff - ph[i].offset);
    }
    im->base = base; im->entry = base + eh.entry; im->phnum = eh.phnum;
    close( fd );
    return 0;
}

extern char **environ;

static void *stack( int argc, char **argv, const image *exe, const image *interp, const char *execfn )
{
    size_t size = 8 << 20;
    char *top = inv_mmap( NULL, size, PROT_READ | PROT_WRITE, 0x20022, -1, 0 );   // + MAP_STACK
    if (top == MAP_FAILED) return NULL;
    char *p = top + size;
    int envc = 0;
    while (environ[envc]) envc++;
    uint64_t *av = calloc( (size_t)argc + 1, sizeof *av );
    uint64_t *ev = calloc( (size_t)envc + 1, sizeof *ev );
    if (!av || !ev) { free( av ); free( ev ); return NULL; }
    for (int i = argc - 1; i >= 0; i--)
    {
        size_t n = strlen( argv[i] ) + 1;
        p -= n; memcpy( p, argv[i], n ); av[i] = (uintptr_t)p;
    }
    for (int i = envc - 1; i >= 0; i--)
    {
        size_t n = strlen( environ[i] ) + 1;
        p -= n; memcpy( p, environ[i], n ); ev[i] = (uintptr_t)p;
    }
    size_t n = strlen( execfn ) + 1;
    p -= n; memcpy( p, execfn, n ); uint64_t execptr = (uintptr_t)p;
    p -= 8; memcpy( p, "aarch64", 8 ); uint64_t platform = (uintptr_t)p;
    p -= 16; arc4random_buf( p, 16 ); uint64_t random = (uintptr_t)p;
    p = (char *)((uintptr_t)p & ~(uintptr_t)15);
    uint64_t aux[] = {
        3, exe->phdr, 4, sizeof(phdr), 5, exe->phnum, 6, (uint64_t)getpagesize(),
        7, interp ? interp->base : 0, 8, 0, 9, exe->entry,
        11, getuid(), 12, geteuid(), 13, getgid(), 14, getegid(),
        15, platform, 16, 0, 17, 100, 23, 0, 25, random, 26, 0,
        31, execptr, 0, 0,
    };
    size_t words = 1 + (size_t)argc + 1 + (size_t)envc + 1 + sizeof aux / sizeof aux[0];
    uint64_t *sp = (uint64_t *)(((uintptr_t)p - words * 8) & ~(uintptr_t)15);
    uint64_t *q = sp;
    *q++ = (uint64_t)argc;
    for (int i = 0; i < argc; i++) *q++ = av[i];
    *q++ = 0;
    for (int i = 0; i < envc; i++) *q++ = ev[i];
    *q++ = 0;
    memcpy( q, aux, sizeof aux );
    free( av ); free( ev );
    return sp;
}

int inv_linux_run( const char *path, int argc, char **argv, const char *guest_root )
{
    if (snprintf( root, sizeof root, "%s", guest_root ) >= (int)sizeof root) { errno = ENAMETOOLONG; return -1; }
    init_ids();
    image exe = {0}, interp = {0};
    if (load( path, &exe )) return -1;
    const image *ld = NULL;
    if (exe.interp[0])
    {
        char resolved[PATH_MAX];
        const char *ldpath = inv_guest_path( exe.interp, resolved );
        if (load( ldpath, &interp )) return -1;
        ld = &interp;
    }
    void *sp = stack( argc, argv, &exe, ld, path );
    if (!sp) return -1;
    struct sigaction sa = {0};
    sa.sa_sigaction = trap;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset( &sa.sa_mask );
    if (sigaction( SIGTRAP, &sa, NULL )) return -1;
    mvr_enter( (void *)(ld ? ld->entry : exe.entry), sp );
}
