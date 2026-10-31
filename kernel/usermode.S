/* Фаза 3: вход в ring3 и аварийный возврат в ядро + три крошечные программы ring3.
 * Написано по спецификации x86_64 (Intel SDM vol.3 §6.14 IRET, §5.8 ring-переходы).
 * Эти инструкции на хосте исполнить нельзя — проверка только на QEMU/железе. */
.intel_syntax noprefix
.section .text

/* uint64_t user_enter(const struct iret_frame *frame, uint64_t user_cr3)
 *   rdi = кадр из 5 qword (rip, cs, rflags, rsp, ss) — статическая память ядра
 *   rsi = физический адрес PML4 пользовательского пространства
 * Сохраняет callee-saved регистры, rsp и rflags ядра, включает пользовательский
 * CR3 и делает iretq в ring3. Управление вернётся ОДНИМ способом: кто-то
 * вызовет user_resume(код) — тогда user_enter "вернётся" вызывающему с этим кодом. */
.global user_enter
user_enter:
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15
    pushfq
    mov [rip + user_saved_rsp], rsp
    cli                         /* между сменой CR3 и iretq нас не должны прервать */
    mov cr3, rsi
    mov rsp, rdi                /* iretq снимает 5 qword прямо с кадра */
    iretq

/* void user_resume(uint64_t code)  — noreturn
 * Вызывается из обработчика прерывания (мы на стеке TSS.rsp0, CR3 ещё пользовательский,
 * но верхняя половина общая, так что ядро видно). Выбрасывает всё, что накопилось на
 * этом стеке, и возвращается из user_enter с rax = code. */
.global user_resume
user_resume:
    mov rsp, [rip + user_saved_rsp]
    popfq
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    mov rax, rdi
    ret

/* ---- Программы ring3 (позиционно-независимый код: только регистры и относительные переходы) ---- */

/* 1. Доказать, что мы в ring3 и стек пользователя рабочий, затем нарушить защиту: hlt в ring3 -> #GP. */
.global user_prog_hlt_start, user_prog_hlt_end
user_prog_hlt_start:
    movabs rax, 0x4C494D4F4E4F5333      /* 'LIMONOS3' */
    push rax                            /* запись в стек пользователя */
    pop  rcx                            /* ...и чтение обратно */
0:  hlt                                 /* привилегированная инструкция -> #GP(0) из ring3 */
user_prog_hlt_end:
.section .rodata
.global user_prog_hlt_fault_off
.align 8
user_prog_hlt_fault_off: .quad 0b - user_prog_hlt_start
.section .text

/* 2. Прочитать память ядра из ring3: страница есть, но без бита U -> #PF с кодом P|U (0x5). */
.global user_prog_pf_start, user_prog_pf_end
user_prog_pf_start:
    movabs rax, 0xFFFFFFFF80000000      /* начало образа ядра */
0:  mov rbx, [rax]
user_prog_pf_end:
.section .rodata
.global user_prog_pf_fault_off
.align 8
user_prog_pf_fault_off: .quad 0b - user_prog_pf_start
.section .text

/* 3. Бесконечный цикл с включёнными прерываниями: ядро должно отобрать управление по таймеру. */
.global user_prog_spin_start, user_prog_spin_end
user_prog_spin_start:
1:  jmp 1b
user_prog_spin_end:


/* 4. hello: write(1,msg) ; brk(0) ; brk(+8КиБ) ; запись/чтение кучи ; exit(42). Любой сбой -> exit(1). */
.global user_prog_hello_start, user_prog_hello_end
user_prog_hello_start:
    mov  eax, 1                         /* SYS_write */
    mov  edi, 1                         /* fd = stdout */
    lea  rsi, [rip + .Lhello_msg]
    mov  edx, .Lhello_msg_end - .Lhello_msg
    int  0x80
    mov  eax, 12                        /* SYS_brk(0) -> текущий brk */
    xor  edi, edi
    int  0x80
    mov  rbx, rax                       /* начало кучи */
    lea  rdi, [rbx + 0x2000]
    mov  eax, 12                        /* SYS_brk(+8 КиБ): ядро должно отобразить 2 страницы */
    int  0x80
    lea  rcx, [rbx + 0x2000]
    cmp  rax, rcx
    jne  .Lhello_bad
    movabs rdx, 0x1122334455667788
    mov  [rbx + 0x1FF8], rdx            /* последняя qword второй страницы кучи */
    mov  [rbx], rdx                     /* и первая страница */
    mov  rsi, [rbx + 0x1FF8]
    cmp  rsi, rdx
    jne  .Lhello_bad
    mov  rsi, [rbx]
    cmp  rsi, rdx
    jne  .Lhello_bad
    mov  edi, 42
    mov  eax, 60                        /* SYS_exit(42) */
    int  0x80
.Lhello_bad:
    mov  edi, 1
    mov  eax, 60
    int  0x80
.Lhello_msg:
    .ascii "Привет из ring3!\n"
.Lhello_msg_end:
user_prog_hello_end:

/* 5. write() с указателем на память ядра -> ядро обязано ответить -EFAULT (14), программа выходит с кодом 14 */
.global user_prog_efault_start, user_prog_efault_end
user_prog_efault_start:
    mov  eax, 1
    mov  edi, 1
    movabs rsi, 0xFFFFFFFF80000000
    mov  edx, 16
    int  0x80
    neg  rax
    mov  edi, eax
    mov  eax, 60
    int  0x80
user_prog_efault_end:

/* 6. несуществующий номер вызова -> -ENOSYS (38), программа выходит с кодом 38 */
.global user_prog_nosys_start, user_prog_nosys_end
user_prog_nosys_start:
    mov  eax, 9999
    int  0x80
    neg  rax
    mov  edi, eax
    mov  eax, 60
    int  0x80
user_prog_nosys_end:

.section .data
.global user_saved_rsp
.align 8
user_saved_rsp: .quad 0

.section .note.GNU-stack,"",@progbits
