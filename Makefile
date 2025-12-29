# Настройки
ARCH := x86_64
CC := gcc
LD := ld
OBJCOPY := objcopy

# Пути
BUILDDIR := build
ISODIR := $(BUILDDIR)/iso
EFI_APP := $(BUILDDIR)/BOOTX64.EFI
ISO_IMAGE := $(BUILDDIR)/lufiraos.iso

# Флаги для загрузчика (EFI)
BOOT_CFLAGS := -ffreestanding -fpic -fshort-wchar -mno-red-zone \
               -I/usr/include/efi -I/usr/include/efi/$(ARCH) \
               -I./src/include -Wall -Wextra -O2

# Флаги для ядра
KERNEL_CFLAGS := -ffreestanding -nostdlib -mno-red-zone \
                 -I./src/include -I./src/kernel \
                 -Wall -Wextra -O2 -mcmodel=large

# Флаги линковки
BOOT_LDFLAGS := -nostdlib -T /usr/lib/elf_$(ARCH)_efi.lds \
                -shared -Bsymbolic -L/usr/lib

KERNEL_LDFLAGS := -nostdlib -T src/kernel/linker.ld

# Библиотеки
BOOT_LIBS := -lefi -lgnuefi

# Файлы
BOOT_SRC := src/boot/main.c
BOOT_OBJ := $(BUILDDIR)/boot.o
KERNEL_SRC := src/kernel/kernel.c
KERNEL_OBJ := $(BUILDDIR)/kernel.o

.PHONY: all clean run qemu debug iso

all: $(ISO_IMAGE)

# Создаём директории
$(BUILDDIR):
	mkdir -p $(BUILDDIR) $(ISODIR)/EFI/BOOT

# Компиляция загрузчика
$(BOOT_OBJ): $(BOOT_SRC) | $(BUILDDIR)
	$(CC) $(BOOT_CFLAGS) -c $< -o $@

# Компиляция ядра
$(KERNEL_OBJ): $(KERNEL_SRC) | $(BUILDDIR)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

# Линковка EFI приложения (только загрузчик)
$(BUILDDIR)/main.elf: $(BOOT_OBJ)
	$(LD) $(BOOT_LDFLAGS) /usr/lib/crt0-efi-$(ARCH).o $^ -o $@ $(BOOT_LIBS)

# Конвертация в EFI формат
$(EFI_APP): $(BUILDDIR)/main.elf
	$(OBJCOPY) -j .text -j .sdata -j .data -j .rodata \
	           -j .dynamic -j .dynsym -j .rel* \
	           --target=efi-app-$(ARCH) $< $@

# Создание FAT образа
$(BUILDDIR)/fat.img: $(EFI_APP)
	dd if=/dev/zero of=$@ bs=1024 count=1440
	mformat -i $@ -f 1440 ::
	mmd -i $@ ::/EFI ::/EFI/BOOT
	mcopy -i $@ $< ::/EFI/BOOT/BOOTX64.EFI

# Создание ISO
$(ISO_IMAGE): $(BUILDDIR)/fat.img
	xorriso -as mkisofs -R -f -e fat.img -no-emul-boot -o $@ $<

# Запуск
run: $(ISO_IMAGE)
	qemu-system-x86_64 \
		-bios /usr/share/ovmf/OVMF.fd \
		-cdrom $(ISO_IMAGE) \
		-net none \
		-serial stdio \
		-monitor none

# Графический режим
qemu: $(ISO_IMAGE)
	qemu-system-x86_64 \
		-bios /usr/share/ovmf/OVMF.fd \
		-cdrom $(ISO_IMAGE) \
		-net none \
		-vga std

# Отладка
debug: $(ISO_IMAGE)
	qemu-system-x86_64 \
		-bios /usr/share/ovmf/OVMF.fd \
		-cdrom $(ISO_IMAGE) \
		-net none \
		-serial stdio \
		-s -S

# Очистка
clean:
	rm -rf $(BUILDDIR)

info:
	@echo "LufiraOS Build System"
	@echo "Architecture: $(ARCH)"
	@echo "Build directory: $(BUILDDIR)"