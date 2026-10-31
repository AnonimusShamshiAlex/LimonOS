#ifndef LIMON_SCHED_H
#define LIMON_SCHED_H
#include <stdint.h>
#include <stddef.h>
#include "arch.h"
#include "term.h"

/*
 * Фаза 7: настоящая вытесняющая многозадачность между несколькими ELF-процессами.
 *
 * В отличие от Фазы 5/6 (кнопка «run <файл>» — один процесс, ядро ждёт его целиком),
 * здесь несколько процессов одновременно СОдержатся в памяти (каждый — свой PML4,
 * своя куча, свои дескрипторы), и таймер (IRQ0, 100 Гц) реально переключает между
 * ними по кругу каждые QUANTUM_TICKS тиков — ПОЛНЫЙ снимок регистров и CR3 меняются
 * прямо внутри обработчика прерывания (interrupts.c), до того, как isr_common
 * сделает iretq. Это тот же трюк, которым настоящие ядра делают context switch.
 *
 * Честное ограничение этой фазы: запустить очередь («go») — это один вызов ядра,
 * который блокирует ТЕКУЩУЮ команду шелла, пока вся очередь не опустеет (процессы
 * внутри неё при этом по-настоящему чередуются). Добавлять новые задачи, пока «go»
 * уже выполняется, и печатать в шелле одновременно с работающими процессами —
 * нельзя: для этого нужно встраивать диспетчер в главный цикл ядра (опрос
 * клавиатуры), а не в одну блокирующую команду. Это следующий шаг, не этот.
 */

#define SCHED_MAX_PROC 4

enum proc_state { PROC_FREE = 0, PROC_RUNNABLE, PROC_DONE, PROC_FAULTED, PROC_TIMEOUT };

struct proc_info {
    int slot;
    enum proc_state state;
    char name[24];
    unsigned quanta;          /* сколько раз процесс получал квант времени — доказательство, что чередование настоящее */
    int exit_code;
    const char *fault_desc;   /* для PROC_FAULTED/PROC_TIMEOUT */
};

/* Ставит ELF-файл (file, len — содержимое, как из vfs_data/vfs_len) в очередь. Не запускает:
 * просто занимает слот (PROC_RUNNABLE). -1 нет свободных слотов, -2 плохой ELF, -3..-6 не хватило
 * памяти/не собрался стек (см. exec_err_str через sched_spawn_err). Возвращает номер слота (>=0). */
int sched_spawn(const uint8_t *file, size_t len, int argc, const char *const *argv);
const char *sched_spawn_err(int code);     /* текст последней ошибки sched_spawn по-русски */

int  sched_list(struct proc_info *out, int max);   /* сколько занятых слотов записано в out (включая выполняющиеся) */

/* Запускает/продолжает диспетчер: берёт все PROC_RUNNABLE слоты и крутит их по кругу, пока
 * все не завершатся (сами, крахом или по общему лимиту времени). Блокируется. Печатает лог
 * чередования квантов и вывод/итог каждого процесса, затем освобождает все слоты. */
void sched_run_queue(struct term *t);

/* Хуки из interrupts.c (isr_handler) — вызываются, только если планировщик сейчас активен
 * (interrupts.c уже проверяет cs_is_user(r->cs) до вызова, как и для user_on_*). */
void sched_on_timer(struct regs *r);
void sched_on_exception(struct regs *r);
void sched_on_syscall(struct regs *r);
int  sched_active(void);

#endif
