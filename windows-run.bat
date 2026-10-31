@echo off
chcp 65001 >nul
cd /d "%~dp0"
echo ==============================================
echo   LimonOS: сборка и запуск в VirtualBox
echo ==============================================
where wsl >nul 2>&1
if errorlevel 1 goto nowsl
wsl -u root -e true >nul 2>&1
if errorlevel 1 goto nodistro

echo.
echo [1/2] Собираю образ в WSL. Первый раз это несколько минут.
wsl -u root bash scripts/build-in-wsl.sh
if errorlevel 1 goto buildfail
if not exist "LimonOS.iso" goto buildfail

echo.
echo [2/2] Создаю виртуальную машину в VirtualBox и запускаю.
call "%~dp0scripts\vbox-create.bat"
goto end

:nowsl
echo.
echo Не найден WSL. Он нужен, чтобы собрать систему.
echo Откройте PowerShell от имени администратора и выполните:
echo     wsl --install -d Ubuntu
echo Затем перезагрузите компьютер, один раз откройте Ubuntu из меню Пуск
echo (задайте имя и пароль) и снова запустите этот файл.
goto end

:nodistro
echo.
echo WSL есть, но Ubuntu не установлена или не настроена.
echo Выполните в PowerShell:   wsl --install -d Ubuntu
echo Один раз откройте Ubuntu из меню Пуск, потом запустите этот файл снова.
goto end

:buildfail
echo.
echo Сборка не удалась. Прокрутите окно вверх и найдите строку с error.
echo Пришлите её текст, я помогу.
goto end

:end
echo.
pause
