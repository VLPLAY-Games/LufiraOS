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

# Проверка наличия необходимых утилит
REQUIRED_TOOLS := gcc ld objcopy dd mkfs.fat mmd mcopy qemu-system-x86_64
$(foreach tool,$(REQUIRED_TOOLS),\
    $(if $(shell which $(tool) 2>/dev/null),,\
        $(error "Required tool '$(tool)' not found in PATH")))

# Создаем директории
$(shell mkdir -p $(BUILD_DIR) \
    $(BUILD_DIR)/boot \
    $(BUILD_DIR)/kernel/drivers \
    $(BUILD_DIR)/kernel/shell \
    $(BUILD_DIR)/kernel/system \
    $(BUILD_DIR)/kernel/fs)

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

# Исходные файлы загрузчика
BOOTLOADER_SOURCES := $(wildcard $(BOOTLOADER_DIR)/*.c)
BOOTLOADER_OBJECTS := $(patsubst $(BOOTLOADER_DIR)/%.c,$(BUILD_DIR)/boot/%.o,$(BOOTLOADER_SOURCES))

# Исходные файлы ядра
KERNEL_C_SOURCES := \
    $(KERNEL_DIR)/kernel.c \
    $(KERNEL_DIR)/drivers/console.c \
    $(KERNEL_DIR)/drivers/keyboard.c \
    $(KERNEL_DIR)/shell/shell.c \
    $(KERNEL_DIR)/system/commands.c \
    $(KERNEL_DIR)/system/gdt.c \
    $(KERNEL_DIR)/system/idt.c \
    $(KERNEL_DIR)/system/irq.c \
    $(KERNEL_DIR)/fs/fat.c

KERNEL_ASM_SOURCES := \
    $(KERNEL_DIR)/system/interrupts.S

KERNEL_C_OBJECTS := $(patsubst $(KERNEL_DIR)/%.c,$(BUILD_DIR)/kernel/%.o,$(KERNEL_C_SOURCES))
KERNEL_ASM_OBJECTS := $(patsubst $(KERNEL_DIR)/%.S,$(BUILD_DIR)/kernel/%.o,$(KERNEL_ASM_SOURCES))
KERNEL_OBJECTS := $(KERNEL_C_OBJECTS) $(KERNEL_ASM_OBJECTS)

# Основные цели
.PHONY: all bootloader kernel disk run clean check-disk debug info quick
all: $(BUILD_DIR)/disk.img

bootloader: $(BUILD_DIR)/BOOTX64.EFI

kernel: $(BUILD_DIR)/kernel.bin

disk: $(BUILD_DIR)/disk.img

# Правила для загрузчика
$(BUILD_DIR)/boot/%.o: $(BOOTLOADER_DIR)/%.c
	@echo "  CC    $<"
	@mkdir -p $(dir $@)
	$(CC) $(BOOTLOADER_CFLAGS) -c -o $@ $<

$(BUILD_DIR)/boot.so: $(BOOTLOADER_OBJECTS)
	@echo "  LD    $@"
	$(LD) $(BOOTLOADER_LDFLAGS) -o $@ $(BOOTLOADER_OBJECTS) -lefi -lgnuefi

$(BUILD_DIR)/BOOTX64.EFI: $(BUILD_DIR)/boot.so
	@echo "  OBJCOPY $@"
	$(OBJCOPY) -j .text -j .sdata -j .data -j .dynamic -j .dynsym \
		-j .rel -j .rela -j .reloc --target=efi-app-x86_64 $< $@
	@echo "  Bootloader size: $$(stat -c%s $@ 2>/dev/null || stat -f%z $@ 2>/dev/null || echo unknown) bytes"

# Правила для ядра
$(BUILD_DIR)/kernel/%.o: $(KERNEL_DIR)/%.c
	@echo "  CC    $<"
	@mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) -o $@ $<

$(BUILD_DIR)/kernel/%.o: $(KERNEL_DIR)/%.S
	@echo "  AS    $<"
	@mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) -x assembler-with-cpp -o $@ $<

$(BUILD_DIR)/kernel.elf: $(KERNEL_OBJECTS) $(KERNEL_DIR)/linker.ld
	@echo "  LD    $@"
	$(LD) $(KERNEL_LDFLAGS) -T $(KERNEL_DIR)/linker.ld -o $@ $(KERNEL_OBJECTS)

$(BUILD_DIR)/kernel.bin: $(BUILD_DIR)/kernel.elf
	@echo "  OBJCOPY $@"
	$(OBJCOPY) -O binary $< $@
	@echo "  Kernel size: $$(stat -c%s $@ 2>/dev/null || stat -f%z $@ 2>/dev/null || echo unknown) bytes"

# Создание образа диска для UEFI
$(BUILD_DIR)/disk.img: $(BUILD_DIR)/BOOTX64.EFI $(BUILD_DIR)/kernel.bin
	@echo "=== Creating disk image ==="
	@rm -f $@
	dd if=/dev/zero of=$@ bs=1M count=64 status=none
	@echo "  Formatting as FAT32..."
	mkfs.fat -F 32 -S 512 $@
	@echo "  Creating EFI/BOOT directory..."
	mmd -i $@ ::/EFI
	mmd -i $@ ::/EFI/BOOT
	@echo "  Copying bootloader to EFI/BOOT/BOOTX64.EFI..."
	mcopy -i $@ $(BUILD_DIR)/BOOTX64.EFI ::/EFI/BOOT/BOOTX64.EFI
	@echo "  Copying kernel to root..."
	mcopy -i $@ $(BUILD_DIR)/kernel.bin ::/kernel.bin
	@echo "  Syncing..."
	sync
	@echo "=== Disk image created: $@ ==="
	@echo "  Size: $$(du -h $@ | cut -f1)"
	@echo "  Contents:"
	@echo "  EFI/BOOT directory:"
	@mdir -i $@ ::/EFI/BOOT/ 2>/dev/null || echo "    ERROR: Cannot read directory"
	@echo "  Root directory:"
	@mdir -i $@ ::/ 2>/dev/null || echo "    ERROR: Cannot read directory"

# Создание образа через скрипт (альтернативный метод)
$(BUILD_DIR)/disk_alt.img: $(BUILD_DIR)/BOOTX64.EFI $(BUILD_DIR)/kernel.bin
	@echo "=== Creating disk image (alternative method) ==="
	@./create_disk.sh $(BUILD_DIR)

# Проверка образа диска
check-disk: $(BUILD_DIR)/disk.img
	@echo "=== Checking disk image ==="
	@echo "File type:"
	@file $@
	@echo ""
	@echo "EFI/BOOT directory:"
	@mdir -i $@ ::/EFI/BOOT/
	@echo ""
	@echo "Root directory:"
	@mdir -i $@ ::/
	@echo ""
	@echo "Bootloader check:"
	@mtype -i $@ ::/EFI/BOOT/BOOTX64.EFI > /dev/null 2>&1 && \
		echo "  Bootloader: OK" || echo "  Bootloader: MISSING!"
	@echo ""
	@echo "Kernel check:"
	@mtype -i $@ ::/kernel.bin > /dev/null 2>&1 && \
		echo "  Kernel: OK" || echo "  Kernel: MISSING!"

# Запуск в QEMU с диском (исправленная версия)
run: $(BUILD_DIR)/disk.img
	@echo "=== Starting QEMU ==="
	qemu-system-x86_64 \
		-bios /usr/share/ovmf/OVMF.fd \
		-drive file=$(BUILD_DIR)/disk.img,format=raw,if=ide,index=0 \
		-m 256M \
		-net none \
		-serial stdio \
		-no-reboot \
		-no-shutdown

# Запуск с отладкой
debug: $(BUILD_DIR)/disk.img
	@echo "=== Starting QEMU with debug output ==="
	qemu-system-x86_64 \
		-bios /usr/share/ovmf/OVMF.fd \
		-drive file=$(BUILD_DIR)/disk.img,format=raw,if=ide,index=0 \
		-m 256M \
		-net none \
		-serial stdio \
		-no-reboot \
		-no-shutdown \
		-d cpu_reset,guest_errors \
		-D $(BUILD_DIR)/qemu_debug.log

# Запуск с монитором QEMU
monitor: $(BUILD_DIR)/disk.img
	@echo "=== Starting QEMU with monitor ==="
	qemu-system-x86_64 \
		-bios /usr/share/ovmf/OVMF.fd \
		-drive file=$(BUILD_DIR)/disk.img,format=raw,if=ide,index=0 \
		-m 256M \
		-net none \
		-serial stdio \
		-monitor telnet:127.0.0.1:4444,server,nowait \
		-no-reboot \
		-no-shutdown

# Очистка
clean:
	@echo "=== Cleaning build directory ==="
	rm -rf $(BUILD_DIR)
	@echo "=== Clean complete ==="

# Показать информацию о сборке
info:
	@echo "=== Build Information ==="
	@echo "Architecture: $(ARCH)"
	@echo "Build directory: $(BUILD_DIR)"
	@echo "Bootloader sources: $(BOOTLOADER_SOURCES)"
	@echo "Kernel C sources: $(KERNEL_C_SOURCES)"
	@echo "Kernel ASM sources: $(KERNEL_ASM_SOURCES)"
	@echo "EFI includes: $(EFI_INC)"
	@echo "EFI lib: $(EFI_LIB)"
	@echo "=== End Information ==="

# Быстрая пересборка только ядра и образа
quick: clean
	@echo "=== Quick rebuild ==="
	@make all