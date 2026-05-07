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
BOOTLOADER_DIR := boot
KERNEL_DIR := kernel

# Создаем директории
$(shell mkdir -p $(BUILD_DIR) \
    $(BUILD_DIR)/kernel/drivers $(BUILD_DIR)/kernel/shell $(BUILD_DIR)/kernel/system)

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
                 -Wall -Wextra -std=gnu11 -c -I$(KERNEL_DIR)

# Флаги для линковки ядра
KERNEL_LDFLAGS := -static -nostdlib -z max-page-size=0x1000 --gc-sections

# Исходные файлы ядра
KERNEL_C_SOURCES := \
    $(KERNEL_DIR)/kernel.c \
    $(KERNEL_DIR)/drivers/console.c \
    $(KERNEL_DIR)/drivers/keyboard.c \
    $(KERNEL_DIR)/shell/shell.c \
    $(KERNEL_DIR)/system/commands.c \
    $(KERNEL_DIR)/system/gdt.c \
    $(KERNEL_DIR)/system/idt.c

KERNEL_ASM_SOURCES := \
    $(KERNEL_DIR)/system/interrupts.S

KERNEL_C_OBJECTS := $(patsubst $(KERNEL_DIR)/%.c,$(BUILD_DIR)/kernel/%.o,$(KERNEL_C_SOURCES))
KERNEL_ASM_OBJECTS := $(patsubst $(KERNEL_DIR)/%.S,$(BUILD_DIR)/kernel/%.o,$(KERNEL_ASM_SOURCES))
KERNEL_OBJECTS := $(KERNEL_C_OBJECTS) $(KERNEL_ASM_OBJECTS)

# Подключаем правила для загрузчика
include boot/Makefile.inc

# Правила для ядра
$(BUILD_DIR)/kernel/%.o: $(KERNEL_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) -o $@ $<

$(BUILD_DIR)/kernel/%.o: $(KERNEL_DIR)/%.S
	@mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) -x assembler-with-cpp -o $@ $<

$(BUILD_DIR)/kernel.elf: $(KERNEL_OBJECTS) $(KERNEL_DIR)/linker.ld
	$(LD) $(KERNEL_LDFLAGS) -T $(KERNEL_DIR)/linker.ld -o $@ $(KERNEL_OBJECTS)

$(BUILD_DIR)/kernel.bin: $(BUILD_DIR)/kernel.elf
	$(OBJCOPY) -O binary $< $@

# Создание образа диска для UEFI
$(BUILD_DIR)/disk.img: $(BUILD_DIR)/BOOTX64.EFI $(BUILD_DIR)/kernel.bin
	dd if=/dev/zero of=$@ bs=1M count=10
	mkfs.fat -F 16 $@
	mmd -i $@ ::/EFI
	mmd -i $@ ::/EFI/BOOT
	mcopy -i $@ $(BUILD_DIR)/BOOTX64.EFI ::/EFI/BOOT/
	mcopy -i $@ $(BUILD_DIR)/kernel.bin ::/

# Запуск в QEMU с диском
run: $(BUILD_DIR)/disk.img
	qemu-system-x86_64 \
		-bios /usr/share/ovmf/OVMF.fd \
		-drive format=raw,file=$(BUILD_DIR)/disk.img \
		-net none \
		-serial stdio \
		-m 64M

# Очистка
clean:
	rm -rf $(BUILD_DIR)

.PHONY: all run clean