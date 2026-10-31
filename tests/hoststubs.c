/* Заглушки памяти для хост-стенда: куча = обычный malloc. */
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
void *kmalloc(size_t n) { return malloc(n); }
void *kcalloc(size_t a, size_t b) { return calloc(a, b); }
void kfree(void *p) { free(p); }
size_t heap_used(void) { return 0; }
uint64_t pmm_alloc(size_t pages) { void *p = calloc(pages, 4096); return (uint64_t)(uintptr_t)p; }
uint64_t pmm_free_bytes(void) { return 0; }
uint64_t pmm_used_bytes(void) { return 0; }
size_t heap_total(void) { return 64u * 1024 * 1024; }
unsigned heap_bad_frees(void) { return 0; }

/* Фаза 3: shell.c зовёт user_selftest(); на хосте ring3 нет */
struct term;
int user_selftest(struct term *t) { (void)t; return 0; }
int user_run_file(struct term *t, int cwd, int argc, const char *const *argv) { (void)t; (void)cwd; (void)argc; (void)argv; return 0; }

/* Фаза 7: shell.c зовёт sched_*; настоящий планировщик (user_enter/CR3) живёт в sched.c,
 * который на хосте не собирается — ring3 там нет (см. исключение sched.c в SRCS). */
enum proc_state { PROC_FREE = 0, PROC_RUNNABLE, PROC_DONE, PROC_FAULTED, PROC_TIMEOUT };
struct proc_info { int slot; enum proc_state state; char name[24]; unsigned quanta; int exit_code; const char *fault_desc; };
int sched_spawn(const uint8_t *file, size_t len, int argc, const char *const *argv) { (void)file; (void)len; (void)argc; (void)argv; return -1; }
const char *sched_spawn_err(int code) { (void)code; return "не поддерживается на хост-стенде"; }
int sched_list(struct proc_info *out, int max) { (void)out; (void)max; return 0; }
void sched_run_queue(struct term *t) { (void)t; }
int sched_active(void) { return 0; }
