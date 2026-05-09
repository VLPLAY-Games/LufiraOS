# Основные настройки
ARCH := x86_64
TARGET := $(ARCH)-elf
CC := gcc
LD := ld
OBJCOPY := objcopy

EFI_INC := /usr/include/efi
EFI_INC_ARCH := /usr/include/efi/$(ARCH)
EFI_LIB := /usr/lib

BUILD_DIR := build
BOOTLOADER_DIR := boot
KERNEL_DIR := kernel

REQUIRED_TOOLS := gcc ld objcopy dd mkfs.fat mmd mcopy qemu-system-x86_64
$(foreach tool,$(REQUIRED_TOOLS),\
    $(if $(shell which $(tool) 2>/dev/null),,\
        $(error "Required tool '$(tool)' not found in PATH")))

$(shell mkdir -p $(BUILD_DIR) \
    $(BUILD_DIR)/boot \
    $(BUILD_DIR)/boot/ui \
    $(BUILD_DIR)/boot/system \
    $(BUILD_DIR)/boot/loaders \
    $(BUILD_DIR)/boot/boot_modes \
    $(BUILD_DIR)/kernel/drivers \
    $(BUILD_DIR)/kernel/shell \
    $(BUILD_DIR)/kernel/system \
    $(BUILD_DIR)/kernel/fs)

BOOTLOADER_CFLAGS := -I$(EFI_INC) -I$(EFI_INC_ARCH) \
                     -fpic -ffreestanding -fno-stack-protector \
                     -fshort-wchar -mno-red-zone -Wall \
                     -DEFI_FUNCTION_WRAPPER -std=gnu11

BOOTLOADER_LDFLAGS := -nostdlib -znocombreloc \
                      -T $(EFI_LIB)/elf_$(ARCH)_efi.lds \
                      -shared -Bsymbolic -L$(EFI_LIB) \
                      $(EFI_LIB)/crt0-efi-$(ARCH).o

KERNEL_CFLAGS := -m64 -ffreestanding -fno-stack-protector -fno-stack-check \
                 -fno-asynchronous-unwind-tables -fno-builtin \
                 -mno-red-zone -mgeneral-regs-only \
                 -Wall -Wextra -std=gnu11 -c -I$(KERNEL_DIR)

KERNEL_LDFLAGS := -static -nostdlib -z max-page-size=0x1000 --gc-sections

BOOTLOADER_SOURCES := $(shell find $(BOOTLOADER_DIR) -name '*.c')
BOOTLOADER_OBJECTS := $(patsubst $(BOOTLOADER_DIR)/%.c,$(BUILD_DIR)/boot/%.o,$(BOOTLOADER_SOURCES))

KERNEL_C_SOURCES := \
    $(KERNEL_DIR)/kernel.c \
    $(KERNEL_DIR)/drivers/console.c \
    $(KERNEL_DIR)/drivers/keyboard.c \
	$(KERNEL_DIR)/drivers/mouse.c \
    $(KERNEL_DIR)/shell/shell.c \
    $(KERNEL_DIR)/system/commands.c \
    $(KERNEL_DIR)/system/gdt.c \
    $(KERNEL_DIR)/system/idt.c \
    $(KERNEL_DIR)/system/irq.c \
	$(KERNEL_DIR)/system/disk.c \
    $(KERNEL_DIR)/fs/fat.c \
	$(KERNEL_DIR)/system/pmm.c \
    $(KERNEL_DIR)/system/paging.c \
    $(KERNEL_DIR)/system/heap.c

KERNEL_ASM_SOURCES := \
    $(KERNEL_DIR)/system/interrupts.S

KERNEL_C_OBJECTS := $(patsubst $(KERNEL_DIR)/%.c,$(BUILD_DIR)/kernel/%.o,$(KERNEL_C_SOURCES))
KERNEL_ASM_OBJECTS := $(patsubst $(KERNEL_DIR)/%.S,$(BUILD_DIR)/kernel/%.o,$(KERNEL_ASM_SOURCES))
KERNEL_OBJECTS := $(KERNEL_C_OBJECTS) $(KERNEL_ASM_OBJECTS)

.PHONY: all bootloader kernel disk run clean check-disk debug info quick
all: $(BUILD_DIR)/disk.img

bootloader: $(BUILD_DIR)/BOOTX64.EFI
kernel: $(BUILD_DIR)/kernel.bin
disk: $(BUILD_DIR)/disk.img

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

$(BUILD_DIR)/disk.img: $(BUILD_DIR)/BOOTX64.EFI $(BUILD_DIR)/kernel.bin
	@echo "=== Creating disk image ==="
	@rm -f $@
	dd if=/dev/zero of=$@ bs=1024 count=512 status=none
	@echo "  Formatting as FAT12..."
	mkfs.fat -F 12 -S 512 $@
	@echo "  Creating EFI/BOOT directory..."
	mmd -i $@ ::/EFI
	mmd -i $@ ::/EFI/BOOT
	@echo "  Copying bootloader..."
	mcopy -i $@ $(BUILD_DIR)/BOOTX64.EFI ::/EFI/BOOT/BOOTX64.EFI
	@echo "  Copying kernel..."
	mcopy -i $@ $(BUILD_DIR)/kernel.bin ::/kernel.bin
	@echo "  Creating test directory and file..."
	mmd -i $@ ::/test
	echo "Hello from LufiraOS!" > $(BUILD_DIR)/readme.txt
	mcopy -i $@ $(BUILD_DIR)/readme.txt ::/readme.txt
	rm -f $(BUILD_DIR)/readme.txt
	sync
	@echo "=== Disk image created: $@ ==="

check-disk: $(BUILD_DIR)/disk.img
	@echo "=== Checking disk image ==="
	@file $@
	@echo "EFI/BOOT directory:" && mdir -i $@ ::/EFI/BOOT/
	@echo "Root directory:" && mdir -i $@ ::/
	@echo "Test directory:" && mdir -i $@ ::/test

run: $(BUILD_DIR)/disk.img
	@echo "=== Starting QEMU ==="
	qemu-system-x86_64 \
		-bios /usr/share/ovmf/OVMF.fd \
		-drive file=$(BUILD_DIR)/disk.img,format=raw,if=ide,index=0 \
		-m 64M \
		-net none \
		-serial stdio

debug: $(BUILD_DIR)/disk.img
	qemu-system-x86_64 \
		-bios /usr/share/ovmf/OVMF.fd \
		-drive file=$(BUILD_DIR)/disk.img,format=raw,if=ide,index=0 \
		-m 256M -net none -serial stdio -no-reboot -no-shutdown \
		-d cpu_reset,guest_errors -D $(BUILD_DIR)/qemu_debug.log

monitor: $(BUILD_DIR)/disk.img
	qemu-system-x86_64 \
		-bios /usr/share/ovmf/OVMF.fd \
		-drive file=$(BUILD_DIR)/disk.img,format=raw,if=ide,index=0 \
		-m 256M -net none -serial stdio \
		-monitor telnet:127.0.0.1:4444,server,nowait \
		-no-reboot -no-shutdown

clean:
	@echo "=== Cleaning ==="
	rm -rf $(BUILD_DIR)

info:
	@echo "=== Build Information ==="
	@echo "Architecture: $(ARCH)"
	@echo "Build directory: $(BUILD_DIR)"
	@echo "Bootloader sources: $(BOOTLOADER_SOURCES)"
	@echo "Kernel C sources: $(KERNEL_C_SOURCES)"
	@echo "Kernel ASM sources: $(KERNEL_ASM_SOURCES)"

quick: clean all