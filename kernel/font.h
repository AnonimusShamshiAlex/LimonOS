#ifndef LIMON_FONT_H
#define LIMON_FONT_H
#include <stdint.h>

#define FONT_W 8
#define FONT_H 16

/* Возвращает FONT_H байт: по одному на строку, старший бит — левый пиксель.
 * Поддерживаются ASCII (0x20-0x7E) и русские буквы. */
const uint8_t *font_glyph(uint32_t codepoint);

#endif
