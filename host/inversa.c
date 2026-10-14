// inversa: run Linux programs on macOS. ARM64 ones natively (Klepton + the mvr-native glibc
// layer), x86-64 ones through FEX built for that same layer, over Inversa's Linux kernel.
//   inversa run <program> [args...]
//   inversa probe <program>          load it and list what doesn't resolve
#include "../kernel/kernel.h"
#include "deps.h"
#include "glibc/glibc.h"
#include "tls/mvr_tls.h"
#include "klepton.h"
#include <Security/Security.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern char **environ;
extern void mvr_enter( void *entry, void *sp ) __attribute__((noreturn));
void inv_cpu_feature_registers( char *out, size_t cap );
int inv_linux_run( const char *path, int argc, char **argv, const char *guest_root );
extern int inv_arm64_mode;
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

/* lib/ beside the program, or beside the Inversa.app it sits in */
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

/* the initial process stack the kernel would build: argc, argv, envp, auxv */
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
    *p++ = 0; *p++ = 0;   /* AT_NULL */
    return sp;
}

/* Vulkan: KosmicKrisp through the Vulkan loader (see mvr-native); INVERSA_VULKAN_DRIVER=moltenvk opts out */
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

/* a 4K-page re-exec without the entitlement is killed by the kernel, so ask first */
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

static int usage(void)
{
    fprintf( stderr, "usage: inversa run <program> [args...]\n       inversa probe <program>\n" );
    return 2;
}

int main( int argc, char **argv )
{
    if (argc < 3) return usage();
    int probe = !strcmp( argv[1], "probe" );
    if (!probe && strcmp( argv[1], "run" )) return usage();
    /* probing loads and resolves only: no constructors, so no guest code runs */
    if (probe) setenv( "KL_NO_INIT", "1", 1 );
    const char *prog = argv[2];
    int machine = elf_machine( prog );
    if (machine != EM_AARCH64 && machine != EM_X86_64)
    {
        fprintf( stderr, "inversa: %s is not a Linux ARM64 or x86-64 program\n", prog );
        return 1;
    }

    if (getpagesize() != 0x1000 && !getenv( "INVERSA_NO_4K" ) && !has_xarch_entitlement())
        fprintf( stderr, "inversa: not signed with the cross-architecture entitlement: 16K pages, no memory below 4 GB\n" );
    else if (getpagesize() != 0x1000 && !getenv( "INVERSA_NO_4K" ))
    {
        /* Linux code expects 4K pages, and MAP_JIT at a fixed address needs them: re-exec
         * ourselves as a 4K-page process (needs the cross-architecture entitlement) */
        posix_spawnattr_t attr;
        posix_spawnattr_init( &attr );
        posix_spawnattr_setflags( &attr, POSIX_SPAWN_SETEXEC );
        posix_spawnattr_set_4k_page_size_np( &attr );
        char exe[1024]; uint32_t n = sizeof exe;
        if (!_NSGetExecutablePath( exe, &n )) posix_spawn( NULL, exe, NULL, &attr, argv, environ );
        fprintf( stderr, "inversa: no 4K pages (unsigned build?), continuing with %dK\n", getpagesize() / 1024 );
    }
    /* linked for macOS 11 so XNU keeps x18 (see mvr-native) */
    setenv( "KL_X18", "0", 0 );
    vulkan_env();

    char dir[1100], lib[1200];
    lib_dir( dir, sizeof dir );
    const char *load = prog;
    if (machine == EM_X86_64)
    {
        const char *fex = getenv( "INVERSA_FEX" );
        if (!fex) { snprintf( lib, sizeof lib, "%s/libFEXInversa.so", dir ); fex = lib; }
        load = fex;
        inv_x86_mode = 1;
        char regs[512];
        inv_cpu_feature_registers( regs, sizeof regs );
        setenv( "FEX_CPUFEATUREREGISTERS", regs, 0 );
    }
    /* Inversa's home is lib/'s parent: lib/arm64-root holds the arm64 libraries FEX links
     * against, rootfs/x86_64 the x86-64 guest root */
    char path[1300];
    if (!getenv( "MVR_LIBPATH" ))
    {
        snprintf( path, sizeof path, "%s/arm64-root/usr/lib/aarch64-linux-gnu", dir );
        setenv( "MVR_LIBPATH", path, 0 );
    }
    snprintf( path, sizeof path, "%s/../rootfs/x86_64", dir );
    if (machine == EM_X86_64 && !access( path, R_OK )) setenv( "FEX_ROOTFS", path, 0 );

    if (mvr_tls_init()) return 1;
    mvr_glibc_init();
    inv_kernel_init();
    mvr_glibc_set_exe( load );
    mvr_deps_init( load );
    kl_thread_init();
    mvr_tls_thread_init();

    if (machine == EM_AARCH64 && getenv( "INVERSA_ARM64_LINUX" ))
    {
        char guest_root[1300];
        const char *configured = getenv( "INVERSA_ROOTFS" );
        if (configured) snprintf( guest_root, sizeof guest_root, "%s", configured );
        else snprintf( guest_root, sizeof guest_root, "%s/../rootfs/aarch64", dir );
        inv_arm64_mode = 1;
        if (!getenv( "INVERSA_NO_CRASH_HANDLER" )) mvr_crash_init();
        if (inv_linux_run( prog, argc - 2, argv + 2, guest_root ))
            fprintf( stderr, "inversa: ARM64 loader failed: %s\n", strerror( errno ) );
        return 1;
    }

    kl_image *img = kl_load_recursive( load );
    if (!img) { fprintf( stderr, "inversa: %s\n", kl_error() ); return 1; }
    if (probe || getenv( "INVERSA_REPORT" ))
    {
        unsigned n = 0;
        const char *const *missing = kl_missing_imports( img, &n );
        fprintf( stderr, "inversa: %s loaded, %u unresolved\n", load, n );
        for (unsigned i = 0; i < n; i++) fprintf( stderr, "  unresolved: %s\n", missing[i] );
        unsigned w = 0;
        const char *const *weak = kl_weak_imports( img, &w );
        for (unsigned i = 0; i < w; i++) fprintf( stderr, "  weak, null: %s\n", weak[i] );
        if (probe) return 0;
    }

    if (!getenv( "INVERSA_NO_CRASH_HANDLER" )) mvr_crash_init();
    if (machine == EM_AARCH64)
    {
        void *entry = kl_image_entry( img );
        if (!entry) { fprintf( stderr, "inversa: %s has no entry point\n", prog ); return 1; }
        mvr_enter( entry, build_stack( argc - 2, argv + 2 ) );
    }
    int (*fex_main)( int, char **, char ** ) = (int (*)( int, char **, char ** ))kl_sym( img, "inversa_fex_main" );
    if (!fex_main) { fprintf( stderr, "inversa: %s has no inversa_fex_main\n", load ); return 1; }
    char **fargv = calloc( (size_t)argc, sizeof *fargv );
    fargv[0] = (char *)"FEX";
    for (int i = 2; i < argc; i++) fargv[i - 1] = argv[i];
    return fex_main( argc - 1, fargv, environ );
}
