#!/bin/bash
# Проверки без железа и без эмулятора: формат вывода, игры, FAT32, куча/сборка ядра, снимки рабочего стола.
# Запуск:  tests/run_host_tests.sh   (нужны gcc и python3 с Pillow для PNG)
set -e
cd "$(dirname "$0")/.."
T=/tmp/limon_tests; mkdir -p $T
echo "== format (%.47s и др.)"; gcc -Ikernel tests/format_host.c kernel/format.c kernel/string.c -o $T/fmt && $T/fmt
echo "== игры (ASan+UBSan)"; gcc -Wall -Wextra -O1 -fsanitize=address,undefined -Ikernel tests/games_host.c kernel/games.c -o $T/games && $T/games
echo "== FAT32 (образ собирает tests/mkfat32.py)"; python3 tests/mkfat32.py $T/fat.img
gcc -Wall -Wextra -g -fsanitize=address,undefined -Ikernel tests/fat_host.c kernel/fat32.c -o $T/fat && ASAN_OPTIONS=detect_leaks=0 $T/fat $T/fat.img $T/fat.img.big
echo "== ядро собирается без предупреждений"; make -s 2>&1 | grep -E "warning|error" && exit 1 || true
echo "== снимки рабочего стола -> $T/shot_*.ppm"
SRCS=$(ls kernel/*.c | grep -v -E "kernel/(kernel|mem|interrupts|gdt|idt|pic|ps2|mouse|string|user|sched)\.c")
gcc -std=gnu11 -O1 -w -DHOST_TEST -Ikernel tests/desktop_shot.c tests/hoststubs.c kernel/string.c kernel/mouse.c $SRCS -o $T/shot && (cd /tmp && $T/shot 1024 768)
echo "ВСЁ ОК"

echo "== Memoria/VFS (ASan+UBSan)"
gcc -Wall -Wextra -g -fsanitize=address,undefined -Ikernel tests/vfs_host.c kernel/vfs.c kernel/format.c kernel/string.c -o $T/vfs
ASAN_OPTIONS=detect_leaks=0 $T/vfs

echo "== Проводник/Диспетчер задач -> $T/shot_files_*.ppm, shot_taskmgr.ppm"
gcc -std=gnu11 -O1 -w -DHOST_TEST -Ikernel tests/desktop_shot2.c tests/hoststubs.c kernel/string.c kernel/mouse.c $SRCS -o $T/shot2 && (cd /tmp && $T/shot2 1024 768)
echo "ВСЁ ОК (2)"

echo "== ELF64-загрузчик, фаза 1: разбор/проверка (ASan+UBSan) -- docs/USERSPACE_PLAN.md"
gcc -nostdlib -static -no-pie tests/tiny_static.S -o $T/tiny_static
gcc -Wall -Wextra -g -fsanitize=address,undefined -Ikernel tests/elf_host.c kernel/elf.c kernel/string.c -o $T/elf_test
ASAN_OPTIONS=detect_leaks=0 $T/elf_test $T/tiny_static
echo "ELF OK (2)"

echo "== Таблицы страниц на процесс, фаза 2: map/unmap/translate/share (ASan+UBSan) -- docs/USERSPACE_PLAN.md"
gcc -Wall -Wextra -g -fsanitize=address,undefined -DHOST_TEST -Ikernel tests/vmm_host.c kernel/vmm.c kernel/string.c -o $T/vmm_test
ASAN_OPTIONS=detect_leaks=0 $T/vmm_test
echo "VMM OK (2)"

echo "== GDT/TSS и ring3, фаза 3: дескрипторы, кадр iretq, разбор #PF, склейка с vmm.c (ASan+UBSan) -- docs/USERSPACE_PLAN.md"
gcc -Wall -Wextra -g -fsanitize=address,undefined -DHOST_TEST -Ikernel tests/gdt_host.c kernel/usermode.S kernel/vmm.c kernel/string.c -o $T/gdt_test
ASAN_OPTIONS=detect_leaks=0 $T/gdt_test
echo "RING3 (чистая часть) OK (3)"

echo "== Системные вызовы, фаза 4: диспетчер write/brk/exit/read на поддельной памяти (ASan+UBSan) -- docs/USERSPACE_PLAN.md"
gcc -Wall -Wextra -g -fsanitize=address,undefined -Ikernel tests/syscall_host.c kernel/syscall.c kernel/usermode.S -o $T/sys_test
ASAN_OPTIONS=detect_leaks=0 $T/sys_test
echo "SYSCALL (чистая часть) OK (4)"

echo "== Мини-libc, фаза 5: та же библиотека, собранная с syscall, запускается как НАСТОЯЩИЙ процесс Linux (hello + стресс-тест malloc/strings)"
UCF="-std=gnu11 -O2 -Wall -Wextra -Werror -ffreestanding -fno-stack-protector -fno-pic -fno-pie -m64 -mno-sse -mno-mmx -mno-80387 -fno-tree-loop-distribute-patterns -fno-builtin -Iuserland"
mkdir -p $T/uh
for f in sys string malloc fmt stdio hello; do gcc $UCF -DLIMON_HOST_SYSCALL -c userland/$f.c -o $T/uh/$f.o; done
gcc $UCF -DLIMON_HOST_SYSCALL -c userland/tests/libc_test.c -o $T/uh/libc_test.o
gcc $UCF -c userland/crt0.S -o $T/uh/crt0.o
HL="$T/uh/sys.o $T/uh/string.o $T/uh/malloc.o $T/uh/fmt.o $T/uh/stdio.o"
ld -nostdlib -static -T userland/user.ld $T/uh/crt0.o $HL $T/uh/hello.o -o $T/uh/hello_host.elf
ld -nostdlib -static -T userland/user.ld $T/uh/crt0.o $HL $T/uh/libc_test.o -o $T/uh/libc_test
$T/uh/libc_test
echo "LIBC OK (5)"
echo "== vsnprintf против glibc: тысячи комбинаций флагов/ширины/точности/длины (ASan+UBSan)"
gcc -Wall -g -fsanitize=address,undefined -Iuserland tests/fmt_diff_host.c -o $T/fmt_diff
ASAN_OPTIONS=detect_leaks=0 $T/fmt_diff
echo "== Загрузчик ELF, фаза 5: настоящий hello.elf в страницы процесса + сквозной запуск загруженного образа (ASan+UBSan)"
make -s build/userland/hello.elf
gcc -Wall -Wextra -g -fsanitize=address,undefined -Ikernel tests/exec_host.c kernel/exec.c kernel/elf.c -o $T/exec_test
ASAN_OPTIONS=detect_leaks=0 $T/exec_test build/userland/hello.elf $T/uh/hello_host.elf
echo "EXEC OK (6)"

echo "== Криптография для SSH (SHA-512, Ed25519-проверка, ChaCha20-Poly1305@openssh, AES-CTR, P-256 ECDH) против cryptography/hashlib"
python3 tests/test_crypto_ssh.py
echo "== TLS 1.2 и возобновление сессии -> tests/run_tls_test.sh (внутри этого скрипта уже гонялось 1.3)"
echo "== FTP/FTPS: настоящий клиент kernel/ftp.c против мок-серверов (открытый, принудительный TLS, vsftpd-режим с возобновлением, только TLS1.3, неявный FTPS)"
python3 tests/test_ftp.py
