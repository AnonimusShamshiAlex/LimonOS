#!/bin/bash
# Собирает LimonOS.iso внутри WSL (Ubuntu). Запускается из windows-run.bat.
set -e
cd "$(dirname "$0")/.."
export DEBIAN_FRONTEND=noninteractive
echo "== Ставлю инструменты (первый раз займёт пару минут) =="
apt-get update -y
apt-get install -y build-essential xorriso git
echo "== Собираю образ =="
make iso
echo "== Готово: LimonOS.iso =="
