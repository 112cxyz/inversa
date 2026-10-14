// AAPCS64 variadic entry points for glibc-only functions (see Klepton's
// kl_va_thunks.S for the frame layout): spill x0-x7/q0-q7, build a va_list over
// them and call the C handler with it after the named arguments.
.text
.macro MVR_VTHUNK name, handler, ngp
    .globl _\name
    .p2align 2
_\name:
    stp     x29, x30, [sp, #-16]!
    mov     x29, sp
    sub     sp, sp, #224
    stp     q0, q1, [sp, #0]
    stp     q2, q3, [sp, #32]
    stp     q4, q5, [sp, #64]
    stp     q6, q7, [sp, #96]
    stp     x0, x1, [sp, #128]
    stp     x2, x3, [sp, #144]
    stp     x4, x5, [sp, #160]
    stp     x6, x7, [sp, #176]
    add     x9,  sp, #192
    add     x10, x29, #16
    str     x10, [x9, #0]
    add     x10, sp, #192
    str     x10, [x9, #8]
    add     x10, sp, #128
    str     x10, [x9, #16]
    mov     w11, #(-64 + (\ngp * 8))
    str     w11, [x9, #24]
    mov     w11, #-128
    str     w11, [x9, #28]
    mov     x\ngp, x9
    bl      _\handler
    add     sp, sp, #224
    ldp     x29, x30, [sp], #16
    ret
.endm

MVR_VTHUNK mvrv_printf_chk,   mvrh_printf_chk,   2
MVR_VTHUNK mvrv_fprintf_chk,  mvrh_fprintf_chk,  3
MVR_VTHUNK mvrv_asprintf_chk, mvrh_asprintf_chk, 3
MVR_VTHUNK mvrv_syslog_chk,   mvrh_syslog_chk,   3
MVR_VTHUNK mvrv_warn,         mvrh_warn,         1
MVR_VTHUNK mvrv_errx,         mvrh_errx,         2
