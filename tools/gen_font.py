#!/usr/bin/env python3
"""Генератор растрового шрифта 8x16 для LimonOS (kernel/font.c).
Рисует символы из TTF-шрифта (по умолчанию DejaVu Sans Mono) без сглаживания.
Запуск:  python3 tools/gen_font.py [путь_к_ttf]
Нужен пакет Pillow:  pip install pillow
"""
import sys
from PIL import Image, ImageDraw, ImageFont

TTF = sys.argv[1] if len(sys.argv) > 1 else "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"
W, H, SIZE = 8, 16, 13

# диапазоны кодовых точек: (начало, конец включительно)
# ASCII, символы Latin-1, русские буквы, стрелки, блоки и пр.
RANGES = [(0x20, 0x7E), (0xAB, 0xAB), (0xB0, 0xB0), (0xBB, 0xBB), (0xD7, 0xD7),
          (0x0401, 0x0401), (0x0410, 0x044F), (0x0451, 0x0451),
          (0x2013, 0x2014), (0x2022, 0x2022), (0x2026, 0x2026), (0x2116, 0x2116),
          (0x2190, 0x2193), (0x2580, 0x2580), (0x2584, 0x2584), (0x2588, 0x2588),
          (0x2591, 0x2593)]

# Блочные символы рисуем вручную, чтобы они заполняли ячейку целиком
def special(cp):
    if cp == 0x2580: return [0xFF] * 8 + [0x00] * 8          # верхняя половина
    if cp == 0x2584: return [0x00] * 8 + [0xFF] * 8          # нижняя половина
    if cp == 0x2588: return [0xFF] * 16                      # сплошной блок
    if cp == 0x2591: return [0x88, 0x22] * 8                 # светлая штриховка
    if cp == 0x2592: return [0xAA, 0x55] * 8                 # средняя
    if cp == 0x2593: return [0xDD, 0x77] * 8                 # тёмная
    return None

SS = 8                      # суперсэмплинг: рисуем крупно, потом уменьшаем
BIG = 20 * SS               # размер шрифта при рисовании (до уменьшения)
font = ImageFont.truetype(TTF, BIG)
ascent, descent = font.getmetrics()
adv = font.getlength("M")   # ширина символа моноширинного шрифта

def glyph(cp):
    sp = special(cp)
    if sp is not None:
        return sp
    # рисуем в большом холсте, затем растягиваем/сжимаем ровно до 8x16 и бинаризуем
    cw, ch = int(round(adv)), ascent + descent
    img = Image.new("L", (cw, ch), 0)
    ImageDraw.Draw(img).text((0, 0), chr(cp), font=font, fill=255)
    # вписываем строку (ascent+descent) в 14 строк + 2 строки запаса сверху/снизу
    small = img.resize((W, 15), Image.BOX)
    canvas = Image.new("L", (W, H), 0)
    canvas.paste(small, (0, 1))
    rows = []
    for y in range(H):
        b = 0
        for x in range(W):
            if canvas.getpixel((x, y)) >= 90:
                b |= 0x80 >> x
        rows.append(b)
    return rows

out = []
out.append("/* АВТОМАТИЧЕСКИ СГЕНЕРИРОВАНО tools/gen_font.py — не редактировать вручную. */")
out.append('#include "font.h"')
out.append("")
out.append("static const uint8_t font_data[][FONT_H] = {")
index = []
n = 0
for lo, hi in RANGES:
    for cp in range(lo, hi + 1):
        rows = glyph(cp)
        out.append("    {" + ",".join("0x%02X" % r for r in rows) + "}, /* U+%04X %s */" % (cp, chr(cp)))
        index.append((cp, n))
        n += 1
out.append("};")
out.append("")
out.append("static const struct { uint32_t lo, hi, first; } font_ranges[] = {")
n = 0
for lo, hi in RANGES:
    out.append("    {0x%04X, 0x%04X, %d}," % (lo, hi, n))
    n += hi - lo + 1
out.append("};")
out.append("")
out.append("const uint8_t *font_glyph(uint32_t cp) {")
out.append("    for (unsigned i = 0; i < sizeof(font_ranges) / sizeof(font_ranges[0]); i++)")
out.append("        if (cp >= font_ranges[i].lo && cp <= font_ranges[i].hi)")
out.append("            return font_data[font_ranges[i].first + (cp - font_ranges[i].lo)];")
out.append("    return font_data['?' - 0x20]; /* неизвестный символ */")
out.append("}")
open("kernel/font.c", "w").write("\n".join(out) + "\n")
print("kernel/font.c записан, глифов:", n)
