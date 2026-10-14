// static x86-64 Linux: write(1, msg, len); exit_group(42)
        .globl _start
        .text
_start:
        movl    $1, %eax
        movl    $1, %edi
        leaq    msg(%rip), %rsi
        movl    $(msg_end - msg), %edx
        syscall
        movl    $231, %eax
        movl    $42, %edi
        syscall
        .section .rodata
msg:    .ascii  "hello from x86-64 Linux, via FEX on Inversa\n"
msg_end:
