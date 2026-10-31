#ifndef LIMON_MEM_H
#define LIMON_MEM_H
#include <stddef.h>
#include <stdint.h>
#include "limine.h"

/* Физическая память: простой выдающий менеджер страниц + куча kmalloc */
void     mem_init(uint64_t hhdm_offset, struct limine_memmap_response *map);
void    *phys_to_virt(uint64_t phys);
uint64_t pmm_alloc(size_t pages);          /* физический адрес, память обнулена; 0 — нет места */
uint64_t pmm_free_bytes(void);
uint64_t pmm_used_bytes(void);

void   heap_init(size_t pages);
void  *kmalloc(size_t n);
void   kfree(void *p);
size_t heap_used(void);

void  *kcalloc(size_t cnt, size_t sz);   /* kmalloc + обнуление, без переполнения cnt*sz */
size_t heap_total(void);
unsigned heap_bad_frees(void);        /* счётчик пойманных плохих/двойных kfree */
#endif
