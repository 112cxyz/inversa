// ELF TLS for glibc-built guests, behind Klepton's kl_tls_hooks.
//
// Guest code reads its thread pointer from TPIDRRO_EL0 (Klepton rewrites
// tpidr_el0, which Darwin uses for the CPU number), so TP is the Darwin TSD base
// and TSD slot k lives at TP + 8k. Two kinds of module:
//   dynamic - reached only through TLSDESC: a per-thread block, found through the
//             table pointer in TSD slot MVR_TLS_TABLE_SLOT, allocated on first use;
//   fixed   - has TLS_TPREL64 (initial-exec) relocations, so its variables need a
//             constant TP offset: they live inside TSD slots MVR_TLS_FIXED_SLOT..,
//             copied in for every thread by mvr_tls_thread_init().
#ifndef MVR_TLS_H
#define MVR_TLS_H
#include <stddef.h>
#include <stdint.h>

#define MVR_TLS_TABLE_SLOT  501
#define MVR_TLS_FIXED_SLOT  502
#define MVR_TLS_FIXED_SLOTS 10      /* 80 bytes for initial-exec TLS */
#define MVR_TLS_MAX_MODULES 256

int  mvr_tls_init(void);            // claim the TSD slots and install kl_tls_hooks
void mvr_tls_thread_init(void);     // every thread, before guest code runs on it
int  mvr_tls_thread_atexit(void (*dtor)(void *), void *obj, void *dso);  // __cxa_thread_atexit_impl
const char *mvr_tls_error(void);
#endif
