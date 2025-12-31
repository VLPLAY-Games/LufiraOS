# Основные настройки
ARCH := x86_64
TARGET := $(ARCH)-elf
CC := gcc
LD := ld
OBJCOPY := objcopy

# Пути к GNU-EFI
EFI_INC := /usr/include/efi
EFI_INC_ARCH := /usr/include/efi/$(ARCH)
EFI_LIB := /usr/lib

# Директории
BUILD_DIR := build
BOOTLOADER_DIR := bootloader
KERNEL_DIR := kernel
ISO_DIR := $(BUILD_DIR)/iso

# Создаем директории
$(shell mkdir -p $(BUILD_DIR) $(ISO_DIR)/boot/grub $(ISO_DIR)/EFI/BOOT)

# Флаги для загрузчика
BOOTLOADER_CFLAGS := -I$(EFI_INC) -I$(EFI_INC_ARCH) \
                     -fpic -ffreestanding -fno-stack-protector \
                     -fshort-wchar -mno-red-zone -Wall \
                     -DEFI_FUNCTION_WRAPPER -std=gnu11

BOOTLOADER_LDFLAGS := -nostdlib -znocombreloc \
                      -T $(EFI_LIB)/elf_$(ARCH)_efi.lds \
                      -shared -Bsymbolic -L$(EFI_LIB) \
                      $(EFI_LIB)/crt0-efi-$(ARCH).o

# Флаги для ядра
KERNEL_CFLAGS := -m64 -ffreestanding -fno-stack-protector -fno-stack-check \
                 -fno-asynchronous-unwind-tables -fno-builtin \
                 -mno-red-zone -mgeneral-regs-only \
                 -Wall -Wextra -std=gnu11 -c

KERNEL_LDFLAGS := -static -nostdlib -z max-page-size=0x1000 --gc-sections

# Цели
all: $(BUILD_DIR)/myos.iso

# Подключаем правила для загрузчика
include bootloader/Makefile.inc

# Правила для ядра
$(BUILD_DIR)/kernel.o: $(KERNEL_DIR)/kernel.c
	$(CC) $(KERNEL_CFLAGS) -o $@ $<

$(BUILD_DIR)/kernel.elf: $(BUILD_DIR)/kernel.o $(KERNEL_DIR)/linker.ld
	$(LD) $(KERNEL_LDFLAGS) -T $(KERNEL_DIR)/linker.ld -o $@ $<

$(BUILD_DIR)/kernel.bin: $(BUILD_DIR)/kernel.elf
	$(OBJCOPY) -O binary $< $@

# Подготовка файлов для ISO
$(ISO_DIR)/EFI/BOOT/BOOTX64.EFI: $(BUILD_DIR)/BOOTX64.EFI
	cp $< $@

$(ISO_DIR)/kernel.bin: $(BUILD_DIR)/kernel.bin
	cp $< $@

# Grub конфигурация для UEFI
$(ISO_DIR)/boot/grub/grub.cfg:
	echo 'set timeout=5' > $@
	echo 'set default=0' >> $@
	echo '' >> $@
	echo 'menuentry "MyOS" {' >> $@
	echo '  echo "Loading MyOS..."' >> $@
	echo '  chainloader /EFI/BOOT/BOOTX64.EFI' >> $@
	echo '}' >> $@

# Создание гибридного ISO (UEFI + BIOS)
$(BUILD_DIR)/myos.iso: $(ISO_DIR)/EFI/BOOT/BOOTX64.EFI $(ISO_DIR)/kernel.bin $(ISO_DIR)/boot/grub/grub.cfg
	grub-mkrescue -o $@ $(ISO_DIR) --modules="part_gpt part_msdos fat normal boot linux configfile chain"

# Создание образа диска для UEFI (альтернативный способ)
$(BUILD_DIR)/disk.img: $(BUILD_DIR)/BOOTX64.EFI $(BUILD_DIR)/kernel.bin
	dd if=/dev/zero of=$@ bs=1M count=10
	mkfs.fat -F 16 $@
	mmd -i $@ ::/EFI
	mmd -i $@ ::/EFI/BOOT
	mcopy -i $@ $(BUILD_DIR)/BOOTX64.EFI ::/EFI/BOOT/
	mcopy -i $@ $(BUILD_DIR)/kernel.bin ::/

# Запуск в QEMU с ISO
run-iso: $(BUILD_DIR)/myos.iso
	qemu-system-x86_64 \
		-bios /usr/share/ovmf/OVMF.fd \
		-cdrom $(BUILD_DIR)/myos.iso \
		-net none \
		-serial stdio \
		-m 256M \
		-no-reboot

# Запуск в QEMU с диском
run-disk: $(BUILD_DIR)/disk.img
	qemu-system-x86_64 \
		-bios /usr/share/ovmf/OVMF.fd \
		-drive format=raw,file=$(BUILD_DIR)/disk.img \
		-net none \
		-serial stdio \
		-m 256M \

# Быстрый запуск
run: run-disk

# Отладка
debug: $(BUILD_DIR)/myos.iso
	qemu-system-x86_64 \
		-bios /usr/share/ovmf/OVMF.fd \
		-cdrom $(BUILD_DIR)/myos.iso \
		-net none \
		-serial stdio \
		-s -S \
		-m 256M \
		-no-reboot

# Проверка содержимого ISO
check-iso: $(BUILD_DIR)/myos.iso
	xorriso -indev $(BUILD_DIR)/myos.iso -report_el_torito as_mkisofs

# Очистка
clean:
	rm -rf $(BUILD_DIR)

.PHONY: all run run-iso run-disk debug check-iso clean