#ifndef LIMON_ELF_H
#define LIMON_ELF_H
#include <stdint.h>
#include <stddef.h>

/*
 * Фаза 1 «настоящего apt»: разбор и ПРОВЕРКА ELF64-файла.
 * Это ещё не загрузка и не запуск — просто честный, протестированный парсер,
 * на который дальше ляжет загрузчик (Фаза 2: таблицы страниц на процесс,
 * Фаза 3: GDT/TSS + переход в ring3, Фаза 4: системные вызовы).
 * См. docs/USERSPACE_PLAN.md — там план целиком и зачем именно так.
 */

#define ELF_MAX_PHDR 16      /* верхняя граница — защита от файла с «миллионом» program header'ов */

enum elf_err {
    ELF_OK = 0,
    ELF_ERR_TOO_SHORT,       /* файл короче e_ehsize */
    ELF_ERR_MAGIC,           /* нет 0x7F 'E' 'L' 'F' */
    ELF_ERR_CLASS,           /* не ELFCLASS64 */
    ELF_ERR_DATA,            /* не little-endian */
    ELF_ERR_VERSION,         /* не EV_CURRENT */
    ELF_ERR_MACHINE,         /* не EM_X86_64 */
    ELF_ERR_TYPE,            /* не ET_EXEC (PIE/ET_DYN пока не поддержан — Фаза 1 только статика) */
    ELF_ERR_PHNUM,           /* 0 или больше ELF_MAX_PHDR program header'ов */
    ELF_ERR_PHOFF,           /* таблица program header'ов вне файла */
    ELF_ERR_NO_LOAD,         /* нет ни одного PT_LOAD сегмента — нечего грузить */
    ELF_ERR_SEGMENT,         /* сегмент выходит за пределы файла или filesz > memsz */
    ELF_ERR_ENTRY,           /* e_entry не попадает ни в один исполняемый PT_LOAD */
    ELF_ERR_OVERLAP,         /* PT_LOAD сегменты пересекаются по виртуальным адресам */
};

#define PT_LOAD 1

#define PF_X 1
#define PF_W 2
#define PF_R 4

struct elf64_segment {
    uint64_t vaddr;    /* куда класть в памяти процесса */
    uint64_t memsz;    /* сколько занимает в памяти (>= filesz, хвост — нулями, под .bss) */
    uint64_t fileoff;  /* откуда брать в файле */
    uint64_t filesz;   /* сколько реально есть в файле */
    uint32_t flags;    /* PF_R/PF_W/PF_X */
};

struct elf64_info {
    uint64_t entry;               /* точка входа (виртуальный адрес) */
    uint64_t lo, hi;               /* минимальный/максимальный виртуальный адрес среди PT_LOAD (для оценки размера образа) */
    int      nseg;                 /* сколько PT_LOAD сегментов реально нашли */
    struct elf64_segment seg[ELF_MAX_PHDR];
};

/* Разбирает и полностью проверяет ELF64 исполняемый файл в буфере buf[0..len).
 * При ELF_OK заполняет *out: точку входа и список PT_LOAD сегментов, уже
 * провалидированных (в границах файла, без пересечений). Ничего не грузит в
 * память и не трогает kmalloc — чистая функция, поэтому тестируется на хосте
 * без эмулятора (tests/elf_host.c). */
enum elf_err elf64_parse(const uint8_t *buf, size_t len, struct elf64_info *out);

/* Текст ошибки на русском — для `run` в шелле, чтобы не выдумывать "program not found"
 * там, где на самом деле файл есть, но он кривой/не наш формат. */
const char *elf64_err_str(enum elf_err e);

#endif
