#!/bin/bash
# Запись LimonOS.iso на USB-флешку. ВНИМАНИЕ: все данные на флешке будут стёрты!
set -e
ISO="${1:-LimonOS.iso}"

[ -f "$ISO" ] || { echo "Нет файла $ISO. Сначала выполни: make iso"; exit 1; }
[ "$(id -u)" -eq 0 ] || { echo "Запусти через sudo: sudo ./scripts/write-usb.sh $ISO"; exit 1; }

echo "Доступные диски:"
lsblk -d -o NAME,SIZE,MODEL,TRAN
echo
read -rp "Введи имя флешки (например sdb, БЕЗ /dev/): " DEV
DEV="/dev/$DEV"
[ -b "$DEV" ] || { echo "Нет такого устройства: $DEV"; exit 1; }

echo
echo "!!! Все данные на $DEV будут УНИЧТОЖЕНЫ !!!"
read -rp "Для подтверждения введи ровно: ДА, ЗАПИСАТЬ $DEV : " CONFIRM
[ "$CONFIRM" = "ДА, ЗАПИСАТЬ $DEV" ] || { echo "Отменено."; exit 1; }

umount "${DEV}"* 2>/dev/null || true
dd if="$ISO" of="$DEV" bs=4M status=progress conv=fsync
sync
echo "Готово. Флешка с LimonOS записана."
