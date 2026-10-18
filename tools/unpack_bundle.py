#!/usr/bin/env python3
"""Восстанавливает проект из одного текстового файла LimonOS_sources.txt.
Запуск:  python3 unpack_bundle.py LimonOS_sources.txt [папка]
Файл kernel/font.c в архив не входит: он создаётся командой  python3 tools/gen_font.py"""
import os, sys, re
src = open(sys.argv[1], encoding="utf-8").read()
out = sys.argv[2] if len(sys.argv) > 2 else "LimonOS"
parts = re.split(r"^={80}\nFILE: (.+)\n={80}\n", src, flags=re.M)
for i in range(1, len(parts), 2):
    path = os.path.join(out, parts[i].strip())
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    open(path, "w", encoding="utf-8").write(parts[i + 1])
    print("записан", path)
