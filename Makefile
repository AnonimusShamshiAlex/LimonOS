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

SRC_C := $(wildcard kernel/*.c)
SRC_S := $(wildcard kernel/*.S)
OBJ   := $(patsubst kernel/%.c,build/%.o,$(SRC_C)) $(patsubst kernel/%.S,build/%_s.o,$(SRC_S))

.PHONY: all iso run run-uefi run-net clean distclean usb vbox

all: $(KERNEL)

build/%.o: kernel/%.c $(wildcard kernel/*.h)
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

build/%_s.o: kernel/%.S
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

$(KERNEL): $(OBJ)
	$(LD) $(LDFLAGS) $(OBJ) -o $@

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
