#include "elf.h"
#include "kstring.h"

/* Нужные нам поля заголовка ELF64 (System V ABI). Мы не включаем полный elf.h
 * из libc (его тут и нет), а читаем сырые байты сами — чтобы не зависеть
 * ни от чего, кроме stdint. */

#define EI_NIDENT 16
#define ELFMAG0 0x7F
#define ELFMAG1 'E'
#define ELFMAG2 'L'
#define ELFMAG3 'F'
#define ELFCLASS64 2
#define ELFDATA2LSB 1
#define EV_CURRENT 1
#define ET_EXEC 2
#define EM_X86_64 62

struct __attribute__((packed)) elf64_ehdr {
    uint8_t  e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

struct __attribute__((packed)) elf64_phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
};

const char *elf64_err_str(enum elf_err e) {
    switch (e) {
    case ELF_OK:          return "ok";
    case ELF_ERR_TOO_SHORT:return "файл короче заголовка ELF";
    case ELF_ERR_MAGIC:   return "не ELF (нет сигнатуры 0x7F ELF)";
    case ELF_ERR_CLASS:   return "не 64-битный ELF";
    case ELF_ERR_DATA:    return "не little-endian ELF";
    case ELF_ERR_VERSION: return "неизвестная версия ELF";
    case ELF_ERR_MACHINE: return "не x86-64 (другая архитектура)";
    case ELF_ERR_TYPE:    return "не статический исполняемый файл (ET_EXEC); PIE/.so пока не поддержаны";
    case ELF_ERR_PHNUM:   return "некорректное число program header'ов";
    case ELF_ERR_PHOFF:   return "таблица program header'ов вне файла";
    case ELF_ERR_NO_LOAD: return "нет ни одного загружаемого (PT_LOAD) сегмента";
    case ELF_ERR_SEGMENT: return "сегмент выходит за пределы файла";
    case ELF_ERR_ENTRY:   return "точка входа вне загружаемых сегментов";
    case ELF_ERR_OVERLAP: return "сегменты пересекаются по адресам";
    }
    return "неизвестная ошибка";
}

static int seg_overlaps(const struct elf64_segment *a, const struct elf64_segment *b) {
    uint64_t a_lo = a->vaddr, a_hi = a->vaddr + a->memsz;
    uint64_t b_lo = b->vaddr, b_hi = b->vaddr + b->memsz;
    return a_lo < b_hi && b_lo < a_hi;
}

enum elf_err elf64_parse(const uint8_t *buf, size_t len, struct elf64_info *out) {
    memset(out, 0, sizeof *out);

    if (len < sizeof(struct elf64_ehdr)) return ELF_ERR_TOO_SHORT;
    const struct elf64_ehdr *eh = (const struct elf64_ehdr *)buf;

    if (eh->e_ident[0] != ELFMAG0 || eh->e_ident[1] != ELFMAG1 ||
        eh->e_ident[2] != ELFMAG2 || eh->e_ident[3] != ELFMAG3) return ELF_ERR_MAGIC;
    if (eh->e_ident[4] != ELFCLASS64) return ELF_ERR_CLASS;
    if (eh->e_ident[5] != ELFDATA2LSB) return ELF_ERR_DATA;
    if (eh->e_ident[6] != EV_CURRENT || eh->e_version != EV_CURRENT) return ELF_ERR_VERSION;
    if (eh->e_machine != EM_X86_64) return ELF_ERR_MACHINE;
    if (eh->e_type != ET_EXEC) return ELF_ERR_TYPE;           /* Фаза 1: только статика, без PIE */
    if (eh->e_phnum == 0 || eh->e_phnum > ELF_MAX_PHDR) return ELF_ERR_PHNUM;
    if (eh->e_phentsize < sizeof(struct elf64_phdr)) return ELF_ERR_PHOFF;

    /* таблица program header'ов должна целиком влезать в файл, без переполнения при умножении */
    uint64_t phtab_bytes = (uint64_t)eh->e_phentsize * eh->e_phnum;
    if (eh->e_phoff > len || phtab_bytes > len - eh->e_phoff) return ELF_ERR_PHOFF;

    out->lo = ~(uint64_t)0;
    out->hi = 0;
    out->entry = eh->e_entry;

    for (int i = 0; i < eh->e_phnum; i++) {
        const struct elf64_phdr *ph = (const struct elf64_phdr *)(buf + eh->e_phoff + (uint64_t)i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD) continue;
        if (ph->p_filesz > ph->p_memsz) return ELF_ERR_SEGMENT;
        if (ph->p_offset > len || ph->p_filesz > len - ph->p_offset) return ELF_ERR_SEGMENT;
        /* защита от переполнения vaddr+memsz на 64-битах */
        if (ph->p_memsz != 0 && ph->p_vaddr + ph->p_memsz < ph->p_vaddr) return ELF_ERR_SEGMENT;

        if (out->nseg >= ELF_MAX_PHDR) return ELF_ERR_PHNUM;   /* не должно случиться — phnum уже ограничен выше */
        struct elf64_segment *s = &out->seg[out->nseg++];
        s->vaddr = ph->p_vaddr;
        s->memsz = ph->p_memsz;
        s->fileoff = ph->p_offset;
        s->filesz = ph->p_filesz;
        s->flags = ph->p_flags;

        if (s->vaddr < out->lo) out->lo = s->vaddr;
        if (s->vaddr + s->memsz > out->hi) out->hi = s->vaddr + s->memsz;
    }

    if (out->nseg == 0) return ELF_ERR_NO_LOAD;

    for (int i = 0; i < out->nseg; i++)
        for (int j = i + 1; j < out->nseg; j++)
            if (seg_overlaps(&out->seg[i], &out->seg[j])) return ELF_ERR_OVERLAP;

    int entry_ok = 0;
    for (int i = 0; i < out->nseg; i++) {
        struct elf64_segment *s = &out->seg[i];
        if ((s->flags & PF_X) && out->entry >= s->vaddr && out->entry < s->vaddr + s->memsz) { entry_ok = 1; break; }
    }
    if (!entry_ok) return ELF_ERR_ENTRY;

    return ELF_OK;
}
