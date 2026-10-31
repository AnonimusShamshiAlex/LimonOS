#ifndef LIMON_IO_H
#define LIMON_IO_H
#include <stdint.h>

/* Порты ввода-вывода и инструкции процессора.
 * При HOST_TEST (проверка на обычном компьютере) это пустые заглушки. */
#ifdef HOST_TEST
static inline void outb(uint16_t p, uint8_t v)  { (void)p; (void)v; }
static inline void outw(uint16_t p, uint16_t v) { (void)p; (void)v; }
static inline uint8_t inb(uint16_t p)           { (void)p; return 0; }
static inline void outl(uint16_t p, uint32_t v) { (void)p; (void)v; }
static inline uint32_t inl(uint16_t p)          { (void)p; return 0; }
static inline void cpu_cli(void) {}
static inline void cpu_sti(void) {}
static inline void cpu_halt(void) {}
#else
static inline void outb(uint16_t p, uint8_t v)  { __asm__ volatile ("outb %0, %1" :: "a"(v), "Nd"(p)); }
static inline void outw(uint16_t p, uint16_t v) { __asm__ volatile ("outw %0, %1" :: "a"(v), "Nd"(p)); }
static inline uint8_t inb(uint16_t p)           { uint8_t v; __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static inline void outl(uint16_t p, uint32_t v) { __asm__ volatile ("outl %0, %1" :: "a"(v), "Nd"(p)); }
static inline uint32_t inl(uint16_t p)          { uint32_t v; __asm__ volatile ("inl %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static inline void cpu_cli(void)  { __asm__ volatile ("cli"); }
static inline void cpu_sti(void)  { __asm__ volatile ("sti"); }
static inline void cpu_halt(void) { __asm__ volatile ("hlt"); }
#endif

#endif
