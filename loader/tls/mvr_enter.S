// mvr_enter(entry, sp): start an ELF executable at e_entry the way the kernel does:
// sp points at argc, argv..., NULL, envp..., NULL, auxv; x0 = rtld_fini = NULL.
    .text
    .p2align 2
    .globl _mvr_enter
_mvr_enter:
    mov     x16, x0
    mov     sp, x1
    mov     x0, xzr
    mov     x29, xzr
    mov     x30, xzr
    br      x16
