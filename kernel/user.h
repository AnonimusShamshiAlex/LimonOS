#ifndef LIMON_USER_H
#define LIMON_USER_H
#include <stdint.h>
#include "arch.h"
#include "ring3.h"
#include "term.h"

enum user_prog { USER_PROG_HLT = 0, USER_PROG_PF = 1, USER_PROG_SPIN = 2,
                 USER_PROG_HELLO = 3, USER_PROG_EFAULT = 4, USER_PROG_NOSYS = 5 };

/* Запускает одну из встроенных ring3-программ в СВОЁМ адресном пространстве
 * (свой PML4, общая верхняя половина с ядром), ждёт, пока ядро заберёт управление
 * (исключение или таймер), возвращает CR3 ядра. 0 — программа реально отработала. */
int user_run(enum user_prog p, struct user_result *out);

/* Три запуска + проверка + вывод в терминал. Возвращает число проваленных проверок. */
int user_selftest(struct term *t);

/* Хуки для isr_handler (interrupts.c): вызываются, только если прерывание пришло из ring3.
 * Если сеанса ring3 нет — user_on_exception вернётся, и ядро запаникует как раньше. */
void user_on_exception(struct regs *r);
void user_on_timer(struct regs *r);
void user_on_syscall(struct regs *r);      /* int 0x80 из ring3 (Фаза 4) */
const char *user_output(void);
/* Фаза 5: запустить настоящий ELF (статический, ET_EXEC) из памяти. 0 — программа реально отработала
 * (смотри out->exit_kind); иначе отрицательный код, а user_setup_msg() — причина по-русски. */
int user_exec(const uint8_t *file, size_t len, int argc, const char *const *argv, struct user_result *out);
const char *user_setup_msg(void);
void user_report(struct term *t, int rc, const struct user_result *r);
void user_register_builtin(void);
int user_run_file(struct term *t, int cwd, int argc, const char *const *argv);              /* что программа вывела через write(1|2) в последнем запуске */

void user_resume(uint64_t code) __attribute__((noreturn));
uint64_t user_enter(const struct iret_frame *frame, uint64_t user_cr3);

#endif
