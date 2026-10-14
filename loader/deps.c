// DT_NEEDED resolution for glibc games, and the host libraries that answer some of
// them directly. glibc's own libraries are served by the glibc layer + Klepton;
// SDL2 and OpenGL go to the macOS builds, so windows, input and GL contexts are
// native ones; everything else is a guest library searched for on disk.
#include "deps.h"
#include "klepton.h"
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char search[16][1024];
static int nsearch;
static void *sdl2, *opengl, *openxr;

static const char *const served[] = {
    "libc.so.6", "libm.so.6", "libpthread.so.0", "libdl.so.2", "librt.so.1",
    "ld-linux-aarch64.so.1", "libutil.so.1",
    "libSDL2-2.0.so.0", "libGL.so.1", "libOpenGL.so.0", "libGLX.so.0", "libvulkan.so.1",
    /* no X server, no udev, no DRM device on macOS: their entry points report "none" */
    "libX11.so.6", "libX11-xcb.so.1", "libxcb.so.1", "libudev.so.1", "libdrm.so.2",
    "libwayland-client.so.0",
    /* the OpenXR runtime a guest's loader opens: MoltenVR's macOS runtime, in-process */
    "libmoltenvr_openxr_host.so",
};

static void add_dir( const char *d )
{
    if (nsearch < 16 && d && *d) snprintf( search[nsearch++], sizeof search[0], "%s", d );
}

static int dep_hook( const char *name, char *full, size_t cap )
{
    for (size_t i = 0; i < sizeof served / sizeof served[0]; i++)
        if (!strcmp( name, served[i] )) return KL_DEP_SERVED;
    if (strchr( name, '/' )) return KL_DEP_DEFAULT;
    for (int i = 0; i < nsearch; i++)
    {
        snprintf( full, cap, "%s/%s", search[i], name );
        if (!access( full, R_OK )) return KL_DEP_PATH;
    }
    return KL_DEP_DEFAULT;
}

static long none(void) { return 0; }

static int is_absent_platform( const char *name )
{
    return (name[0] == 'X' && name[1] >= 'A' && name[1] <= 'Z') || !strncmp( name, "xcb_", 4 ) ||
           !strncmp( name, "udev_", 5 ) || !strncmp( name, "drm", 3 ) || !strncmp( name, "wl_", 3 );
}

/* error dialogs: log them and return, so a headless run can't block on a modal alert */
struct sdl_messagebox { uint32_t flags; void *window; const char *title, *message; };
const char *kl_addr_image( const void *addr, size_t *offset );

static void guest_backtrace(void)
{
    fflush( NULL );
    uint64_t *fp = __builtin_frame_address( 0 );
    for (int i = 0; i < 24 && fp && ((uintptr_t)fp & 7) == 0; i++)
    {
        size_t off = 0;
        const char *img = kl_addr_image( (void *)fp[1], &off );
        if (img) fprintf( stderr, "    #%-2d %s+%#zx\n", i, strrchr( img, '/' ) ? strrchr( img, '/' ) + 1 : img, off );
        fp = (uint64_t *)fp[0];
    }
}
static int msgbox( const struct sdl_messagebox *d, int *button )
{
    fprintf( stderr, "[mvr] message box: %s: %s\n", d->title ? d->title : "", d->message ? d->message : "" );
    guest_backtrace();
    if (button) *button = -1;
    return 0;
}
static int simple_msgbox( uint32_t flags, const char *title, const char *message, void *window )
{
    fprintf( stderr, "[mvr] message box: %s: %s\n", title ? title : "", message ? message : "" );
    guest_backtrace();
    return 0;
}

/* SDL's Vulkan glue, answered here so the guest's surface is created on the same Vulkan
 * (Klepton's MoltenVK) as its instance, instead of whatever loader SDL would find */
typedef void *(*gipa_fn)( void *, const char * );
static gipa_fn klepton_gipa(void) { return (gipa_fn)kl_shim_lookup( "vkGetInstanceProcAddr" ); }
static int sdl_vk_load( const char *path ) { return 0; }
static void *sdl_vk_gipa(void) { return (void *)klepton_gipa(); }
static int sdl_vk_instance_exts( void *window, unsigned *count, const char **names )
{
    static const char *const exts[] = { "VK_KHR_surface", "VK_EXT_metal_surface" };
    if (!names) { *count = 2; return 1; }
    unsigned n = *count < 2 ? *count : 2;
    for (unsigned i = 0; i < n; i++) names[i] = exts[i];
    *count = n;
    return n == 2;
}
struct metal_surface_ci { int sType; const void *pNext; unsigned flags; const void *pLayer; };
static int sdl_vk_create_surface( void *window, void *instance, uint64_t *surface )
{
    void *(*create_view)( void * ) = mvr_hostlib_lookup( "SDL_Metal_CreateView" );
    void *(*get_layer)( void * ) = mvr_hostlib_lookup( "SDL_Metal_GetLayer" );
    void *view = create_view ? create_view( window ) : NULL;
    void *layer = view && get_layer ? get_layer( view ) : NULL;
    if (!layer) { fprintf( stderr, "[mvr] SDL_Vulkan_CreateSurface: no CAMetalLayer for window %p\n", window ); return 0; }
    struct metal_surface_ci ci = { 1000217000 /* VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT */, NULL, 0, layer };
    int (*create)( void *, const void *, const void *, uint64_t * ) =
        (int (*)( void *, const void *, const void *, uint64_t * ))klepton_gipa()( instance, "vkCreateMetalSurfaceEXT" );
    int r = create ? create( instance, &ci, NULL, surface ) : -1;
    if (r) fprintf( stderr, "[mvr] vkCreateMetalSurfaceEXT failed: %d\n", r );
    return r == 0;
}
static void sdl_vk_drawable_size( void *window, int *w, int *h )
{
    void (*get)( void *, int *, int * ) = mvr_hostlib_lookup( "SDL_Metal_GetDrawableSize" );
    if (get) get( window, w, h );
}

void *mvr_hostlib_lookup( const char *name )
{
    if (!strcmp( name, "SDL_Vulkan_LoadLibrary" )) return (void *)sdl_vk_load;
    if (!strcmp( name, "SDL_Vulkan_GetVkGetInstanceProcAddr" )) return (void *)sdl_vk_gipa;
    if (!strcmp( name, "SDL_Vulkan_GetInstanceExtensions" )) return (void *)sdl_vk_instance_exts;
    if (!strcmp( name, "SDL_Vulkan_CreateSurface" )) return (void *)sdl_vk_create_surface;
    if (!strcmp( name, "SDL_Vulkan_GetDrawableSize" )) return (void *)sdl_vk_drawable_size;
    if (!strcmp( name, "SDL_Vulkan_UnloadLibrary" )) return (void *)none;
    if (!getenv( "MVR_SHOW_DIALOGS" ))
    {
        if (!strcmp( name, "SDL_ShowMessageBox" )) return (void *)msgbox;
        if (!strcmp( name, "SDL_ShowSimpleMessageBox" )) return (void *)simple_msgbox;
    }
    if (is_absent_platform( name )) return (void *)none;
    if (!strcmp( name, "xrNegotiateLoaderRuntimeInterface" ))
    {
        if (!openxr)
        {
            const char *p = getenv( "MVR_OPENXR_RUNTIME" );
            char def[1024];
            snprintf( def, sizeof def, "%s/MoltenVR/openxr-runtime-macos/libmoltenvr_openxr.dylib", getenv( "HOME" ) );
            openxr = dlopen( p ? p : def, RTLD_NOW | RTLD_LOCAL );
            if (!openxr) fprintf( stderr, "[mvr] OpenXR runtime: %s\n", dlerror() );
            /* Vulkan apps using XR_KHR_vulkan_enable: the runtime must reach the same
             * Vulkan (Klepton's MoltenVK) the guest does */
            void (*set_loader)( void * ) = openxr ? (void (*)( void * ))dlsym( openxr, "oxr_vulkan_set_loader" ) : NULL;
            if (set_loader) set_loader( kl_shim_lookup( "vkGetInstanceProcAddr" ) );
        }
        return openxr ? dlsym( openxr, name ) : NULL;
    }
    if (!strncmp( name, "SDL_", 4 ))
    {
        if (!sdl2) sdl2 = dlopen( "/opt/homebrew/lib/libSDL2-2.0.0.dylib", RTLD_NOW | RTLD_LOCAL );
        return sdl2 ? dlsym( sdl2, name ) : NULL;
    }
    if (name[0] == 'g' && name[1] == 'l' && name[2] >= 'A' && name[2] <= 'Z')
    {
        if (!opengl) opengl = dlopen( "/System/Library/Frameworks/OpenGL.framework/OpenGL", RTLD_NOW | RTLD_LOCAL );
        return opengl ? dlsym( opengl, name ) : NULL;
    }
    return NULL;
}

void mvr_deps_init( const char *exe_path )
{
    char dir[1024];
    snprintf( dir, sizeof dir, "%s", exe_path );
    char *slash = strrchr( dir, '/' );
    if (slash) *slash = 0; else snprintf( dir, sizeof dir, "." );
    add_dir( dir );
    char lib[1100];
    snprintf( lib, sizeof lib, "%s/lib", dir ); add_dir( lib );
    snprintf( lib, sizeof lib, "%s/lib/arm64", dir ); add_dir( lib );
    snprintf( lib, sizeof lib, "%s/lib/aarch64", dir ); add_dir( lib );
    const char *extra = getenv( "MVR_LIBPATH" );
    if (extra)
    {
        char *copy = strdup( extra ), *save = NULL;
        for (char *p = strtok_r( copy, ":", &save ); p; p = strtok_r( NULL, ":", &save )) add_dir( p );
        free( copy );
    }
    kl_set_library_path( dir );
    kl_dep_hook = dep_hook;
}
