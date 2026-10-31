CC      := gcc
LD      := ld
NAME    := LimonOS
KERNEL  := build/kernel.elf
ISO     := $(NAME).iso

CFLAGS  := -std=gnu11 -O2 -Wall -Wextra -ffreestanding -fno-stack-protector \
	       -fno-stack-check -fno-lto -fno-pic -fno-pie -m64 -march=x86-64 \
	       -mno-80387 -mno-mmx -mno-sse -mno-sse2 -mno-red-zone \
	       -mcmodel=kernel -fno-tree-loop-distribute-patterns -Ikernel
LDFLAGS := -nostdlib -static -z max-page-size=0x1000 -T kernel/linker.ld
# libgcc.a: чисто программные помощники компилятора (деление/умножение 128-бит и т.п.),
# без зависимостей от ОС — нужна calc.c для __int128 в calc.c (128-битное умножение/деление без потери точности).
LIBGCC  := $(shell $(CC) -m64 -mcmodel=kernel -print-libgcc-file-name)

SRC_C := $(wildcard kernel/*.c)
SRC_S := $(wildcard kernel/*.S)
OBJ   := $(patsubst kernel/%.c,build/%.o,$(SRC_C)) $(patsubst kernel/%.S,build/%_s.o,$(SRC_S))

.DEFAULT_GOAL := all

# ---- Пользовательские программы (Фаза 5): мини-libc + hello.elf, собираются ДО ядра и встраиваются в него ----
UCFLAGS := -std=gnu11 -O2 -Wall -Wextra -ffreestanding -fno-stack-protector -fno-pic -fno-pie -m64 \
           -mno-sse -mno-mmx -mno-80387 -fno-tree-loop-distribute-patterns -fno-builtin -Iuserland
ULDFLAGS := -nostdlib -static -z max-page-size=0x1000 -T userland/user.ld
ULIB     := sys string malloc fmt stdio
UOBJ     := build/userland/crt0.o $(patsubst %,build/userland/%.o,$(ULIB))

build/userland/crt0.o: userland/crt0.S
	@mkdir -p build/userland
	$(CC) $(UCFLAGS) -c $< -o $@

build/userland/%.o: userland/%.c userland/libc.h
	@mkdir -p build/userland
	$(CC) $(UCFLAGS) -c $< -o $@

build/userland/hello.elf: $(UOBJ) build/userland/hello.o userland/user.ld
	$(LD) $(ULDFLAGS) $(UOBJ) build/userland/hello.o -o $@

build/embed_s.o: build/userland/hello.elf

.PHONY: all iso run run-uefi run-net clean distclean usb vbox

all: $(KERNEL)

build/%.o: kernel/%.c $(wildcard kernel/*.h)
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

build/%_s.o: kernel/%.S
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

$(KERNEL): $(OBJ)
	$(LD) $(LDFLAGS) $(OBJ) $(LIBGCC) -o $@

# Загрузчик Limine (скачивается один раз, нужен интернет)
limine:
	git clone https://github.com/limine-bootloader/limine.git --branch=v9.x-binary --depth=1
	$(MAKE) -C limine

iso: $(ISO)

$(ISO): $(KERNEL) limine.conf | limine
	rm -rf iso_root
	mkdir -p iso_root/boot iso_root/EFI/BOOT
	cp $(KERNEL) iso_root/boot/kernel
	cp limine.conf iso_root/boot/limine.conf
	cp limine/limine-bios.sys limine/limine-bios-cd.bin limine/limine-uefi-cd.bin iso_root/boot/
	cp limine/BOOTX64.EFI limine/BOOTIA32.EFI iso_root/EFI/BOOT/
	xorriso -as mkisofs -R -r -J -b boot/limine-bios-cd.bin \
	    -no-emul-boot -boot-load-size 4 -boot-info-table -hfsplus \
	    -apm-block-size 2048 --efi-boot boot/limine-uefi-cd.bin \
	    -efi-boot-part --efi-boot-image --protective-msdos-label \
	    iso_root -o $(ISO)
	./limine/limine bios-install $(ISO)

run: $(ISO)
	qemu-system-x86_64 -cdrom $(ISO) -m 256M

# QEMU с сетевой картой e1000 (как в VirtualBox) и выходом в интернет
run-net: $(ISO)
	qemu-system-x86_64 -cdrom $(ISO) -m 512M -nic user,model=e1000

# создать и запустить виртуальную машину в VirtualBox
vbox: $(ISO)
	./scripts/vbox-create.sh $(ISO)

# запуск в режиме UEFI (нужен пакет ovmf)
run-uefi: $(ISO)
	qemu-system-x86_64 -cdrom $(ISO) -m 256M -bios /usr/share/ovmf/OVMF.fd

usb: $(ISO)
	./scripts/write-usb.sh $(ISO)

clean:
	rm -rf build iso_root $(ISO)

distclean: clean
	rm -rf limine
