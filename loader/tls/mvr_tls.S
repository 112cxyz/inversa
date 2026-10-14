// TLSDESC resolvers. In: x0 = the descriptor (GOT pair). Out: x0 = variable
// address minus the thread pointer. Everything else, NEON included, is preserved.
#define TABLE_SLOT_OFF (501 * 8)

    .text
    .p2align 2
    .globl _mvr_tlsdesc_static
_mvr_tlsdesc_static:
    ldr     x0, [x0, #8]
    ret

    .globl _mvr_tlsdesc_dynamic
_mvr_tlsdesc_dynamic:
    stp     x1, x2, [sp, #-32]!
    str     x3, [sp, #16]
    ldr     x0, [x0, #8]                // desc_arg { index, off }
    mrs     x1, tpidrro_el0
    ldr     x2, [x1, #TABLE_SLOT_OFF]
    cbz     x2, 1f
    ldr     x3, [x0]
    ldr     x2, [x2, x3, lsl #3]
    cbz     x2, 1f
    ldr     x3, [x0, #8]
    add     x0, x2, x3
    sub     x0, x0, x1
    ldr     x3, [sp, #16]
    ldp     x1, x2, [sp], #32
    ret
1:  ldr     x3, [sp, #16]
    ldp     x1, x2, [sp], #32
    sub     sp, sp, #672                // x1-x18, x29, x30, q0-q31
    stp     x1, x2, [sp, #0]
    stp     x3, x4, [sp, #16]
    stp     x5, x6, [sp, #32]
    stp     x7, x8, [sp, #48]
    stp     x9, x10, [sp, #64]
    stp     x11, x12, [sp, #80]
    stp     x13, x14, [sp, #96]
    stp     x15, x16, [sp, #112]
    stp     x17, x18, [sp, #128]
    stp     x29, x30, [sp, #144]
    stp     q0, q1, [sp, #160]
    stp     q2, q3, [sp, #192]
    stp     q4, q5, [sp, #224]
    stp     q6, q7, [sp, #256]
    stp     q8, q9, [sp, #288]
    stp     q10, q11, [sp, #320]
    stp     q12, q13, [sp, #352]
    stp     q14, q15, [sp, #384]
    stp     q16, q17, [sp, #416]
    stp     q18, q19, [sp, #448]
    stp     q20, q21, [sp, #480]
    stp     q22, q23, [sp, #512]
    stp     q24, q25, [sp, #544]
    stp     q26, q27, [sp, #576]
    stp     q28, q29, [sp, #608]
    stp     q30, q31, [sp, #640]
    add     x29, sp, #144
    bl      _mvr_tls_slow
    ldp     x1, x2, [sp, #0]
    ldp     x3, x4, [sp, #16]
    ldp     x5, x6, [sp, #32]
    ldp     x7, x8, [sp, #48]
    ldp     x9, x10, [sp, #64]
    ldp     x11, x12, [sp, #80]
    ldp     x13, x14, [sp, #96]
    ldp     x15, x16, [sp, #112]
    ldp     x17, x18, [sp, #128]
    ldp     x29, x30, [sp, #144]
    ldp     q0, q1, [sp, #160]
    ldp     q2, q3, [sp, #192]
    ldp     q4, q5, [sp, #224]
    ldp     q6, q7, [sp, #256]
    ldp     q8, q9, [sp, #288]
    ldp     q10, q11, [sp, #320]
    ldp     q12, q13, [sp, #352]
    ldp     q14, q15, [sp, #384]
    ldp     q16, q17, [sp, #416]
    ldp     q18, q19, [sp, #448]
    ldp     q20, q21, [sp, #480]
    ldp     q22, q23, [sp, #512]
    ldp     q24, q25, [sp, #544]
    ldp     q26, q27, [sp, #576]
    ldp     q28, q29, [sp, #608]
    ldp     q30, q31, [sp, #640]
    add     sp, sp, #672
    ret
