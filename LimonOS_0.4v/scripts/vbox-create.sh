#!/bin/bash
# Создаёт виртуальную машину LimonOS в VirtualBox и запускает её.
# Нужен собранный LimonOS.iso (make iso) и установленный VirtualBox (команда VBoxManage).
set -e
VM="${VM:-LimonOS}"
ISO="${1:-LimonOS.iso}"

command -v VBoxManage >/dev/null || { echo "VBoxManage не найден. Установи VirtualBox или добавь его в PATH."; exit 1; }
[ -f "$ISO" ] || { echo "Нет файла $ISO. Сначала выполни: make iso"; exit 1; }

# старую машину с таким именем удаляем, чтобы скрипт можно было запускать повторно
if VBoxManage showvminfo "$VM" >/dev/null 2>&1; then
    VBoxManage controlvm "$VM" poweroff >/dev/null 2>&1 || true
    sleep 1
    VBoxManage unregistervm "$VM" --delete >/dev/null 2>&1 || true
fi

VBoxManage createvm --name "$VM" --ostype Other_64 --register
VBoxManage modifyvm "$VM" \
    --memory 512 --vram 32 --cpus 1 --ioapic on --firmware bios \
    --graphicscontroller vboxvga \
    --mouse ps2 --keyboard ps2 \
    --nic1 nat --nictype1 82540EM --cableconnected1 on \
    --boot1 dvd --boot2 none --boot3 none --boot4 none
VBoxManage storagectl "$VM" --name SATA --add sata --controller IntelAhci
VBoxManage storageattach "$VM" --storagectl SATA --port 0 --device 0 --type dvddrive --medium "$ISO"

echo "Машина $VM создана (сеть NAT, карта Intel PRO/1000 MT, мышь PS/2). Запускаю..."
VBoxManage startvm "$VM"
