@echo off
chcp 65001 >nul
rem Создаёт виртуальную машину LimonOS в VirtualBox и запускает её (Windows).
set "VBM=%ProgramFiles%\Oracle\VirtualBox\VBoxManage.exe"
if not exist "%VBM%" set "VBM=%VBOX_MSI_INSTALL_PATH%VBoxManage.exe"
if not exist "%VBM%" goto novbox
for %%I in ("%~dp0..\LimonOS.iso") do set "ISO=%%~fI"
if not exist "%ISO%" goto noiso
set VM=LimonOS

"%VBM%" controlvm %VM% poweroff >nul 2>&1
"%VBM%" unregistervm %VM% --delete >nul 2>&1

"%VBM%" createvm --name %VM% --ostype Other_64 --register
if errorlevel 1 goto fail
"%VBM%" modifyvm %VM% --memory 2048 --vram 32 --cpus 2 --ioapic on --firmware bios --graphicscontroller vboxvga --mouse ps2 --keyboard ps2 --nic1 nat --nictype1 82540EM --cableconnected1 on --boot1 dvd --boot2 none --boot3 none --boot4 none
if errorlevel 1 goto fail
"%VBM%" storagectl %VM% --name SATA --add sata --controller IntelAhci
"%VBM%" storageattach %VM% --storagectl SATA --port 0 --device 0 --type dvddrive --medium "%ISO%"
if errorlevel 1 goto fail
rem диск 10 ГБ (заработает после драйвера диска и файловой системы - этап 7)
for %%I in ("%~dp0..\LimonOS-disk.vdi") do set "DISK=%%~fI"
if not exist "%DISK%" "%VBM%" createmedium disk --filename "%DISK%" --size 10240 --format VDI
"%VBM%" storageattach %VM% --storagectl SATA --port 1 --device 0 --type hdd --medium "%DISK%"
echo Машина создана: сеть NAT, карта Intel PRO/1000 MT, мышь PS/2. Запускаю.
"%VBM%" startvm %VM%
goto end

:novbox
echo VirtualBox не найден в Program Files. Установите его или поправьте путь в этом файле.
exit /b 1
:noiso
echo Нет файла LimonOS.iso. Сначала запустите windows-run.bat
exit /b 1
:fail
echo Не удалось создать машину. Текст ошибки выше.
exit /b 1
:end
