// inversa: run Linux programs on macOS. ARM64 ones natively (Klepton + the glibc layer in
// loader/), x86-64 ones through FEX built for that same layer, over Inversa's Linux kernel.
//   inversa run <program> [args...]
//   inversa probe <program>          load it and list what doesn't resolve
#include "../kernel/kernel.h"
#include "deps.h"
#include "entitlement.h"
#include "glibc/glibc.h"
#include "tls/mvr_tls.h"
#include "klepton.h"
#include <errno.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

extern char **environ;
extern void mvr_enter( void *entry, void *sp ) __attribute__((noreturn));
void inv_cpu_feature_registers( char *out, size_t cap );
void mvr_crash_init(void);

enum { EM_AARCH64 = 183, EM_X86_64 = 62 };

static int elf_machine( const char *path )
{
    unsigned char h[20];
    int fd = open( path, O_RDONLY );
    if (fd < 0) return -1;
    ssize_t n = read( fd, h, sizeof h );
    close( fd );
    if (n < 20 || memcmp( h, "\177ELF", 4 )) return 0;
    return h[18] | h[19] << 8;
}

// lib/ beside the program, or beside the Inversa.app it sits in
static void lib_dir( char *out, size_t cap )
{
    char exe[1024], cand[1100]; uint32_t n = sizeof exe;
    out[0] = 0;
    if (_NSGetExecutablePath( exe, &n )) return;
    char *slash = strrchr( exe, '/' ); if (slash) *slash = 0;
    snprintf( cand, sizeof cand, "%s/../lib", exe );
    if (access( cand, R_OK )) snprintf( cand, sizeof cand, "%s/../../../lib", exe );
    snprintf( out, cap, "%s", cand );
}

// the initial process stack the kernel would build: argc, argv, envp, auxv
static void *build_stack( int argc, char **argv )
{
    int envc = 0;
    while (environ[envc]) envc++;
    size_t words = 1 + argc + 1 + envc + 1 + 2, size = 8 << 20;
    char *stack = malloc( size );
    uint64_t *sp = (uint64_t *)(((uintptr_t)(stack + size) - words * 8) & ~(uintptr_t)15), *p = sp;
    *p++ = argc;
    for (int i = 0; i < argc; i++) *p++ = (uint64_t)(uintptr_t)argv[i];
    *p++ = 0;
    for (int i = 0; i < envc; i++) *p++ = (uint64_t)(uintptr_t)environ[i];
    *p++ = 0;
    *p++ = 0; *p++ = 0;   // AT_NULL
    return sp;
}

// INVERSA_TIME=1: the first inversa in an exec chain starts the clock for the rest
static void start_clock(void)
{
    if (!getenv( "INVERSA_TIME" ) || getenv( "INVERSA_T0" )) return;
    struct timespec t; char b[32];
    clock_gettime( CLOCK_MONOTONIC, &t );
    snprintf( b, sizeof b, "%.3f", t.tv_sec * 1e3 + t.tv_nsec / 1e6 );
    setenv( "INVERSA_T0", b, 1 );
}

// Linux code expects 4K pages, and MAP_JIT at a fixed address needs them. The launcher normally
// starts us in a 4K process already; run directly, re-exec ourselves into one
static void ensure_4k_pages( char **argv )
{
    if (getpagesize() == 0x1000 || getenv( "INVERSA_NO_4K" ) || !has_xarch_entitlement()) return;
    posix_spawnattr_t attr;
    posix_spawnattr_init( &attr );
    posix_spawnattr_setflags( &attr, POSIX_SPAWN_SETEXEC );
    posix_spawnattr_set_4k_page_size_np( &attr );
    char exe[1024]; uint32_t n = sizeof exe;
    if (!_NSGetExecutablePath( exe, &n )) posix_spawn( NULL, exe, NULL, &attr, argv, environ );
    fprintf( stderr, "inversa: 4K page re-exec failed; still using %dK pages\n", getpagesize() / 1024 );
}

// Vulkan: KosmicKrisp through the Vulkan loader when Homebrew has both; INVERSA_VULKAN_DRIVER=moltenvk opts out
static void vulkan_env(void)
{
    const char *driver = getenv( "INVERSA_VULKAN_DRIVER" );
    if (getenv( "KL_MVK_DYLIB" ) || getenv( "KL_MVK_DIR" ) || (driver && !strcmp( driver, "moltenvk" ))) return;
    const char *icd = getenv( "VK_DRIVER_FILES" ), *icds[] = { "/opt/homebrew/share/vulkan/icd.d/kosmickrisp_mesa_icd.aarch64.json" };
    if (!icd && !access( icds[0], R_OK )) icd = icds[0];
    if (icd && !access( "/opt/homebrew/lib/libvulkan.1.dylib", R_OK ))
    {
        setenv( "KL_MVK_DYLIB", "/opt/homebrew/lib/libvulkan.1.dylib", 0 );
        setenv( "VK_DRIVER_FILES", icd, 0 );
    }
}

// Inversa's home is lib/'s parent: lib/arm64-root holds the arm64 libraries FEX links against,
// rootfs/x86_64 the x86-64 guest root. Returns what Klepton should load: the program, or FEX.
static const char *guest_env( int machine, const char *prog, const char *dir )
{
    static char fex[1200];
    char path[1300];
    setenv( "KL_X18", "0", 0 );   // guest code uses x18; see the macOS 11 link in the Makefile
    vulkan_env();
    if (!getenv( "MVR_LIBPATH" ))
    {
        snprintf( path, sizeof path, "%s/arm64-root/usr/lib/aarch64-linux-gnu", dir );
        setenv( "MVR_LIBPATH", path, 0 );
    }
    if (machine != EM_X86_64) return prog;

    inv_x86_mode = 1;
    char regs[512];
    inv_cpu_feature_registers( regs, sizeof regs );
    setenv( "FEX_CPUFEATUREREGISTERS", regs, 0 );
    snprintf( path, sizeof path, "%s/../rootfs/x86_64", dir );
    if (!access( path, R_OK )) setenv( "FEX_ROOTFS", path, 0 );
    if (getenv( "INVERSA_FEX" )) return getenv( "INVERSA_FEX" );
    snprintf( fex, sizeof fex, "%s/libFEXInversa.so", dir );
    return fex;
}

static void print_image( const char *name, void *base, size_t span, void *ctx )
{
    fprintf( ctx, "inversa: image %p %p %s\n", base, (char *)base + span, name );
}

static void report_imports( kl_image *img, const char *load )
{
    unsigned n = 0, w = 0;
    const char *const *missing = kl_missing_imports( img, &n );
    fprintf( stderr, "inversa: %s loaded, %u unresolved\n", load, n );
    for (unsigned i = 0; i < n; i++) fprintf( stderr, "  unresolved: %s\n", missing[i] );
    const char *const *weak = kl_weak_imports( img, &w );
    for (unsigned i = 0; i < w; i++) fprintf( stderr, "  weak, null: %s\n", weak[i] );
}

// INVERSA_ARM64_LINUX=1: the program on its own ld.so and glibc, from an ARM64 root
static int run_arm64_linux( const char *prog, int argc, char **argv, const char *dir )
{
    char root[1300];
    if (getenv( "INVERSA_ROOTFS" )) snprintf( root, sizeof root, "%s", getenv( "INVERSA_ROOTFS" ) );
    else snprintf( root, sizeof root, "%s/../rootfs/aarch64", dir );
    inv_arm64_mode = 1;
    if (!getenv( "INVERSA_NO_CRASH_HANDLER" )) mvr_crash_init();
    if (inv_linux_run( prog, argc, argv, root ))
        fprintf( stderr, "inversa: ARM64 loader failed: %s\n", strerror( errno ) );
    return 1;
}

static int run_fex( kl_image *img, const char *load, int argc, char **argv )
{
    int (*fex_main)( int, char **, char ** ) = (int (*)( int, char **, char ** ))kl_sym( img, "inversa_fex_main" );
    if (!fex_main) { fprintf( stderr, "inversa: %s has no inversa_fex_main\n", load ); return 1; }
    char **fargv = calloc( (size_t)argc + 2, sizeof *fargv );
    fargv[0] = (char *)"FEX";
    for (int i = 0; i < argc; i++) fargv[i + 1] = argv[i];
    inv_time_mark( "entering FEX" );
    return fex_main( argc + 1, fargv, environ );
}

static int usage(void)
{
    fprintf( stderr, "usage: inversa run <program> [args...]\n       inversa probe <program>\n" );
    return 2;
}

int main( int argc, char **argv )
{
    start_clock();
    inv_time_mark( "main" );
    if (argc < 3) return usage();
    int probe = !strcmp( argv[1], "probe" );
    if (!probe && strcmp( argv[1], "run" )) return usage();
    // probing loads and resolves only: no constructors, so no guest code runs
    if (probe) setenv( "KL_NO_INIT", "1", 1 );
    const char *prog = argv[2];
    int guest_argc = argc - 2;
    char **guest_argv = argv + 2;
    int machine = elf_machine( prog );
    if (machine != EM_AARCH64 && machine != EM_X86_64)
    {
        fprintf( stderr, "inversa: %s is not a Linux ARM64 or x86-64 program\n", prog );
        return 1;
    }

    ensure_4k_pages( argv );
    if (machine == EM_X86_64 && !probe && getpagesize() != 0x1000)
    {
        fprintf( stderr, "inversa: x86-64 requires 4K pages and the cross-architecture entitlement (current page size: %dK)\n",
                 getpagesize() / 1024 );
        return 1;
    }
    char dir[1100];
    lib_dir( dir, sizeof dir );
    const char *load = guest_env( machine, prog, dir );

    inv_time_mark( "after 4K re-exec" );
    if (mvr_tls_init()) return 1;
    mvr_glibc_init();
    inv_kernel_init();
    mvr_glibc_set_exe( load );
    mvr_deps_init( load );
    kl_thread_init();
    mvr_tls_thread_init();
    if (machine == EM_AARCH64 && getenv( "INVERSA_ARM64_LINUX" ))
        return run_arm64_linux( prog, guest_argc, guest_argv, dir );

    inv_time_mark( "runtime set up" );
    kl_image *img = kl_load_recursive( load );
    inv_time_mark( "loaded, relocated, constructors run" );
    if (!img) { fprintf( stderr, "inversa: %s\n", kl_error() ); return 1; }
    if (getenv( "INVERSA_IMAGES" )) kl_each_image( print_image, stderr );   // to symbolise `sample`
    if (probe || getenv( "INVERSA_REPORT" )) report_imports( img, load );
    if (probe) return 0;

    if (!getenv( "INVERSA_NO_CRASH_HANDLER" )) mvr_crash_init();
    if (machine == EM_X86_64) return run_fex( img, load, guest_argc, guest_argv );
    void *entry = kl_image_entry( img );
    if (!entry) { fprintf( stderr, "inversa: %s has no entry point\n", prog ); return 1; }
    mvr_enter( entry, build_stack( guest_argc, guest_argv ) );
}
