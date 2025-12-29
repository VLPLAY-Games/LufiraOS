# Настройки
ARCH := x86_64
CC := gcc
LD := ld
OBJCOPY := objcopy

# Флаги
CFLAGS := -ffreestanding -fpic -fshort-wchar -mno-red-zone \
          -I/usr/include/efi -I/usr/include/efi/$(ARCH) -I./src/include \
          -Wall -Wextra -O2

LDFLAGS := -nostdlib -znocombreloc -T /usr/lib/elf_$(ARCH)_efi.lds \
           -shared -Bsymbolic -L/usr/lib

LIBS := -lefi -lgnuefi

# Пути
BUILDDIR := build
EFI_APP := $(BUILDDIR)/BOOTX64.EFI
ISO_IMAGE := $(BUILDDIR)/lufiraos.iso
OVMF_CODE := /usr/share/ovmf/OVMF.fd

# Файлы
BOOT_SRC := src/boot/main.c
KERNEL_SRC := src/kernel/kernel.c
BOOT_OBJ := $(BUILDDIR)/boot.o
KERNEL_OBJ := $(BUILDDIR)/kernel.o

.PHONY: all clean run qemu debug

all: $(ISO_IMAGE)

# Создаём директории
$(BUILDDIR):
	mkdir -p $(BUILDDIR)

# Компиляция загрузчика
$(BOOT_OBJ): $(BOOT_SRC) | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Компиляция ядра
$(KERNEL_OBJ): $(KERNEL_SRC) | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Линковка EFI приложения
$(BUILDDIR)/main.elf: $(BOOT_OBJ) $(KERNEL_OBJ)
	$(LD) $(LDFLAGS) /usr/lib/crt0-efi-$(ARCH).o $^ -o $@ $(LIBS)

# Конвертация в EFI формат
$(EFI_APP): $(BUILDDIR)/main.elf
	$(OBJCOPY) -j .text -j .sdata -j .data -j .rodata \
	           -j .dynamic -j .dynsym -j .rel* \
	           --target=efi-app-$(ARCH) $< $@

# Создание загрузочной структуры для ISO - УПРОЩЕННЫЙ РАБОЧИЙ ВАРИАНТ
$(BUILDDIR)/efi.img: $(EFI_APP)
	# Создаём минимальный FAT12 образ (1.44 МБ) - это точно работает
	dd if=/dev/zero of=$@ bs=1024 count=1440
	mkfs.fat -F 12 $@
	
	# Создаём директории и копируем EFI приложение
	mmd -i $@ ::/EFI
	mmd -i $@ ::/EFI/BOOT
	mcopy -i $@ $< ::/EFI/BOOT/BOOTX64.EFI

# Альтернатива: если нужен 2 МБ образ
# $(BUILDDIR)/efi.img: $(EFI_APP)
# 	# Создаём FAT16 образ 2 МБ
# 	dd if=/dev/zero of=$@ bs=1024 count=2048
# 	mkfs.fat -F 16 -s 4 $@
# 	mmd -i $@ ::/EFI
# 	mmd -i $@ ::/EFI/BOOT
# 	mcopy -i $@ $< ::/EFI/BOOT/BOOTX64.EFI

# Создание ISO образа
$(ISO_IMAGE): $(BUILDDIR)/efi.img
	xorriso -as mkisofs -R -f -e /efi.img -no-emul-boot -o $@ $<

# Запуск в QEMU (режим UEFI с выводом в терминал)
run: $(ISO_IMAGE)
	qemu-system-x86_64 \
		-bios $(OVMF_CODE) \
		-cdrom $(ISO_IMAGE) \
		-net none \
		-serial stdio \
		-monitor none \
		-nographic

# Запуск в QEMU с графикой
qemu: $(ISO_IMAGE)
	qemu-system-x86_64 \
		-bios $(OVMF_CODE) \
		-cdrom $(ISO_IMAGE) \
		-net none \
		-vga std \
		-monitor stdio

# Отладка
debug: $(ISO_IMAGE)
	qemu-system-x86_64 \
		-bios $(OVMF_CODE) \
		-cdrom $(ISO_IMAGE) \
		-net none \
		-serial stdio \
		-s -S

# Создание только ISO
iso: $(ISO_IMAGE)

# Очистка
clean:
	rm -rf $(BUILDDIR)

# Информация о проекте
info:
	@echo "LufiraOS Build System"
	@echo "Target: $(ARCH)-uefi"
	@echo "Commands:"
	@echo "  make        - Build everything"
	@echo "  make run    - Run in QEMU (text mode)"
	@echo "  make qemu   - Run in QEMU (graphics)"
	@echo "  make debug  - Run with GDB debugger"
	@echo "  make clean  - Clean build directory"