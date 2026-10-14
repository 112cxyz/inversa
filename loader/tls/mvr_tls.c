#include "mvr_tls.h"
#include "klepton.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const void *init;
    size_t filesz, memsz, align;
    unsigned index;
    int fixed_off;       // TP-relative offset of the block, or -1 for a dynamic module
} module;

typedef struct { uint64_t index, off; } desc_arg;

extern void mvr_tlsdesc_static(void);
extern void mvr_tlsdesc_dynamic(void);

static module modules[MVR_TLS_MAX_MODULES];
static unsigned module_count;
static size_t fixed_used;
static char error_buf[256];

const char *mvr_tls_error(void) { return error_buf; }

static inline uintptr_t thread_pointer(void)
{
    uintptr_t tp;
    __asm__ volatile( "mrs %0, tpidrro_el0" : "=r"(tp) );
    return tp;
}

static inline void **tsd(void) { return (void **)thread_pointer(); }

static void *new_block(const module *m)
{
    size_t align = m->align < 16 ? 16 : m->align;
    void *block = aligned_alloc( align, (m->memsz + align - 1) & ~(align - 1) );
    if (!block) return NULL;
    memcpy( block, m->init, m->filesz );
    memset( (char *)block + m->filesz, 0, m->memsz - m->filesz );
    return block;
}

/* slow path of mvr_tlsdesc_dynamic: this thread has no block for the module yet */
uintptr_t mvr_tls_slow( const desc_arg *arg )
{
    void ***slot = (void ***)&tsd()[MVR_TLS_TABLE_SLOT];
    if (!*slot) *slot = calloc( MVR_TLS_MAX_MODULES, sizeof(void *) );
    void **table = *slot;
    if (!table[arg->index]) table[arg->index] = new_block( &modules[arg->index] );
    if (!table[arg->index]) { fprintf( stderr, "mvr_tls: out of memory\n" ); abort(); }
    return (uintptr_t)table[arg->index] + arg->off - thread_pointer();
}

static void *register_module( const char *path, const void *init, size_t filesz, size_t memsz,
                              size_t align, int fixed )
{
    if (module_count >= MVR_TLS_MAX_MODULES) return NULL;
    module *m = &modules[module_count];
    *m = (module){ init, filesz, memsz, align ? align : 1, module_count, -1 };
    if (fixed)
    {
        size_t base = MVR_TLS_FIXED_SLOT * 8, room = MVR_TLS_FIXED_SLOTS * 8;
        size_t start = (fixed_used + m->align - 1) & ~(m->align - 1);
        if ((base + start) % m->align || start + memsz > room)
        {
            snprintf( error_buf, sizeof error_buf, "%s: %zu bytes of initial-exec TLS (align %zu) don't fit",
                      path, memsz, m->align );
            fprintf( stderr, "mvr_tls: %s\n", error_buf );
            return NULL;
        }
        m->fixed_off = (int)(base + start);
        fixed_used = start + memsz;
    }
    module_count++;
    return m;
}

static void desc( void *mod, uint64_t off, uint64_t slot[2] )
{
    module *m = mod;
    if (m->fixed_off >= 0)
    {
        slot[0] = (uint64_t)(uintptr_t)mvr_tlsdesc_static;
        slot[1] = (uint64_t)m->fixed_off + off;
        return;
    }
    desc_arg *arg = malloc( sizeof *arg );
    *arg = (desc_arg){ m->index, off };
    slot[0] = (uint64_t)(uintptr_t)mvr_tlsdesc_dynamic;
    slot[1] = (uint64_t)(uintptr_t)arg;
}

static int tprel( void *mod, uint64_t off, uint64_t *tp_off )
{
    module *m = mod;
    if (m->fixed_off < 0) return -1;
    *tp_off = (uint64_t)m->fixed_off + off;
    return 0;
}

/* the loading thread gets its copy once the init image is relocated: it can hold pointers */
static void relocated( void *mod )
{
    module *m = mod;
    if (m->fixed_off < 0) return;
    memcpy( (char *)thread_pointer() + m->fixed_off, m->init, m->filesz );
    memset( (char *)thread_pointer() + m->fixed_off + m->filesz, 0, m->memsz - m->filesz );
}

void mvr_tls_thread_init(void)
{
    char *tp = (char *)thread_pointer();
    for (unsigned i = 0; i < module_count; i++)
    {
        const module *m = &modules[i];
        if (m->fixed_off < 0) continue;
        memcpy( tp + m->fixed_off, m->init, m->filesz );
        memset( tp + m->fixed_off + m->filesz, 0, m->memsz - m->filesz );
    }
}

static void free_table( void *p )
{
    void **table = p;
    for (unsigned i = 0; i < MVR_TLS_MAX_MODULES; i++) free( table[i] );
    free( table );
}

/* __cxa_thread_atexit_impl: destructors for thread_local objects, run at thread exit */
typedef struct atexit_entry { void (*dtor)(void *); void *obj; struct atexit_entry *next; } atexit_entry;
static pthread_key_t atexit_key;

static void run_atexit( void *p )
{
    for (atexit_entry *e = p, *next; e; e = next) { next = e->next; e->dtor( e->obj ); free( e ); }
}

int mvr_tls_thread_atexit( void (*dtor)(void *), void *obj, void *dso )
{
    atexit_entry *e = malloc( sizeof *e );
    if (!e) return -1;
    *e = (atexit_entry){ dtor, obj, pthread_getspecific( atexit_key ) };
    pthread_setspecific( atexit_key, e );
    return 0;
}

/* claim exactly the keys we document, the way Klepton claims slot 500 */
static int claim_slots(void)
{
    pthread_key_t held[512]; int n = 0, ok = 1;
    int want_lo = MVR_TLS_TABLE_SLOT, want_hi = MVR_TLS_FIXED_SLOT + MVR_TLS_FIXED_SLOTS - 1;
    int got = 0;
    pthread_key_t k;
    while (got < want_hi - want_lo + 1 && n < 512 && !pthread_key_create( &k, NULL ))
    {
        if ((int)k >= want_lo && (int)k <= want_hi) got++;
        else held[n++] = k;
        if ((int)k > want_hi) { ok = 0; break; }
    }
    for (int i = 0; i < n; i++) pthread_key_delete( held[i] );
    if (!ok || got != want_hi - want_lo + 1)
    {
        snprintf( error_buf, sizeof error_buf, "couldn't claim TSD slots %d-%d", want_lo, want_hi );
        return -1;
    }
    return 0;
}

int mvr_tls_init(void)
{
    if (claim_slots()) { fprintf( stderr, "mvr_tls: %s\n", error_buf ); return -1; }
    pthread_key_create( &atexit_key, run_atexit );
    tsd()[MVR_TLS_TABLE_SLOT] = NULL;
    for (int i = 0; i < MVR_TLS_FIXED_SLOTS; i++) tsd()[MVR_TLS_FIXED_SLOT + i] = NULL;
    kl_tls_hooks = (kl_tls_hooks_t){ register_module, desc, tprel, relocated };
    (void)free_table;
    return 0;
}
