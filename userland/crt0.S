/* Точка входа программы (System V x86-64: на входе rsp -> argc, затем argv[], NULL, envp[], NULL, auxv). */
.intel_syntax noprefix
.section .text
.global _start
_start:
    xor  ebp, ebp                   /* конец цепочки кадров для отладчиков */
    mov  rdi, [rsp]                 /* argc */
    lea  rsi, [rsp + 8]             /* argv */
    lea  rdx, [rsi + rdi*8 + 8]     /* envp = после argv и его NULL */
    and  rsp, -16
    call __libc_start
    ud2
.section .note.GNU-stack,"",@progbits
