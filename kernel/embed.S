/* Встроенный в ядро настоящий ELF-файл userland/hello.elf (собирается Makefile'ом до ядра).
 * user_register_builtin() кладёт его в /System/hello.elf. */
.section .rodata
.balign 16
.global hello_elf_start, hello_elf_end
hello_elf_start:
    .incbin "build/userland/hello.elf"
hello_elf_end:
.section .note.GNU-stack,"",@progbits
