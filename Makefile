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

# Держите в синхроне с LUFIRAFS_ESP_SIZE в
# kernel/fs/lufirafs/lufirafs_format.h — расхождение означает, что mkfs
# отформатирует не тот регион диска, который потом читает ядро.
DISK_TOTAL_SIZE := 16777216
LUFIRAFS_ESP_SIZE := 4194304
LUFIRAFS_REGION_SIZE := $(shell echo $$(($(DISK_TOTAL_SIZE) - $(LUFIRAFS_ESP_SIZE))))

REQUIRED_TOOLS := gcc ld objcopy nm truncate dd mkfs.fat mmd mcopy qemu-system-x86_64
$(foreach tool,$(REQUIRED_TOOLS),\
    $(if $(shell which $(tool) 2>/dev/null),,\
        $(error "Required tool '$(tool)' not found in PATH")))

$(shell mkdir -p $(BUILD_DIR) \
    $(BUILD_DIR)/boot \
    $(BUILD_DIR)/kernel/lib \
	$(BUILD_DIR)/kernel/drivers/pci \
    $(BUILD_DIR)/kernel/drivers/console \
    $(BUILD_DIR)/kernel/drivers/keyboard \
	$(BUILD_DIR)/kernel/drivers/sound \
    $(BUILD_DIR)/kernel/drivers/mouse \
    $(BUILD_DIR)/kernel/drivers/disk \
	$(BUILD_DIR)/kernel/drivers/usb \
	$(BUILD_DIR)/kernel/drivers/input \
    $(BUILD_DIR)/kernel/shell \
	$(BUILD_DIR)/kernel/shell/commands \
    $(BUILD_DIR)/kernel/system/cpu \
    $(BUILD_DIR)/kernel/system/mm \
	$(BUILD_DIR)/kernel/system/acpi \
	$(BUILD_DIR)/kernel/system/timer \
	$(BUILD_DIR)/kernel/system/process \
	$(BUILD_DIR)/kernel/system/syscall \
	$(BUILD_DIR)/kernel/system/elf \
	$(BUILD_DIR)/kernel/fs/vfs \
    $(BUILD_DIR)/kernel/fs/lufirafs)

BOOTLOADER_CFLAGS := -I$(EFI_INC) -I$(EFI_INC_ARCH) \
                     -I$(BOOTLOADER_DIR) \
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

KERNEL_LDFLAGS := -static -nostdlib -z max-page-size=0x1000 -z separate-code --gc-sections

BOOTLOADER_SOURCES := $(shell find $(BOOTLOADER_DIR) -name '*.c')
BOOTLOADER_OBJECTS := $(patsubst $(BOOTLOADER_DIR)/%.c,$(BUILD_DIR)/boot/%.o,$(BOOTLOADER_SOURCES))

KERNEL_C_SOURCES := \
    $(KERNEL_DIR)/kernel.c \
    $(KERNEL_DIR)/lib/string.c \
    $(KERNEL_DIR)/lib/cpu.c \
	$(KERNEL_DIR)/drivers/pci/pci.c \
    $(KERNEL_DIR)/drivers/console/console.c \
    $(KERNEL_DIR)/drivers/keyboard/keyboard.c \
    $(KERNEL_DIR)/drivers/mouse/mouse.c \
    $(KERNEL_DIR)/drivers/disk/disk.c \
	$(KERNEL_DIR)/drivers/sound/ac97.c \
	$(KERNEL_DIR)/drivers/usb/uhci.c \
	$(KERNEL_DIR)/drivers/usb/usb_hid.c \
	$(KERNEL_DIR)/drivers/input/input.c \
    $(KERNEL_DIR)/shell/shell.c \
    $(KERNEL_DIR)/shell/commands/system.c \
    $(KERNEL_DIR)/shell/commands/colors.c \
    $(KERNEL_DIR)/shell/commands/filesystem.c \
	$(KERNEL_DIR)/shell/commands/sound.c \
    $(KERNEL_DIR)/system/cpu/gdt.c \
    $(KERNEL_DIR)/system/cpu/idt.c \
    $(KERNEL_DIR)/system/cpu/irq.c \
    $(KERNEL_DIR)/system/cpu/tss.c \
    $(KERNEL_DIR)/system/mm/pmm.c \
    $(KERNEL_DIR)/system/mm/paging.c \
    $(KERNEL_DIR)/system/mm/heap.c \
    $(KERNEL_DIR)/system/acpi/acpi.c \
	$(KERNEL_DIR)/system/timer/pit.c \
    $(KERNEL_DIR)/system/process/process.c \
	$(KERNEL_DIR)/system/syscall/syscall.c \
	$(KERNEL_DIR)/system/elf/elf.c \
	$(KERNEL_DIR)/fs/vfs/vfs.c \
	$(KERNEL_DIR)/fs/lufirafs/lufirafs.c \
	$(KERNEL_DIR)/fs/lufirafs/lufirafs_vfs.c

KERNEL_ASM_SOURCES := \
    $(KERNEL_DIR)/system/cpu/interrupts.S \
    $(KERNEL_DIR)/system/process/switch.S \
	$(KERNEL_DIR)/system/syscall/syscall_entry.S \

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
	@KERNEL_END=$$(nm $(BUILD_DIR)/kernel.elf | awk '$$3=="__kernel_end"{print $$1}'); \
	KERNEL_SIZE=$$((0x$$KERNEL_END - 0x100000)); \
	echo "  Kernel runtime size: $$KERNEL_SIZE bytes"; \
	echo "  Kernel end: 0x$$KERNEL_END"; \
	truncate -s $$KERNEL_SIZE $@

$(BUILD_DIR)/mkfs_lufirafs: tools/mkfs_lufirafs.c $(KERNEL_DIR)/fs/lufirafs/lufirafs_format.h
	@echo "  CC(host) $<"
	$(CC) -O2 -Wall -o $@ $<

# Диск — два региона без таблицы разделов (bootloader грузит в RAM ВЕСЬ
# диск одним куском начиная с LBA 0, см. LUFIRAFS_ESP_SIZE в
# lufirafs_format.h): первые LUFIRAFS_ESP_SIZE байт — маленький ESP,
# отформатированный как FAT12 обычными mtools (UEFI-прошивка умеет читать
# файлы ТОЛЬКО с FAT — это требование спецификации, не наш выбор), в нём
# лежит ИСКЛЮЧИТЕЛЬНО сам бутлоадер и kernel.bin. Всё остальное место —
# LufiraFS, наша собственная файловая система для всех пользовательских
# данных, размечает и наполняет её $(BUILD_DIR)/mkfs_lufirafs.
#
# Общий размер образа (16МБ) сохранён от прежней FAT-only схемы — в своё
# время меньший образ (512КБ) реально исчерпывал место при сборке
# (mcopy проваливался с ошибкой), 16МБ даёт кратный запас.
$(BUILD_DIR)/disk.img: $(BUILD_DIR)/BOOTX64.EFI $(BUILD_DIR)/kernel.bin $(BUILD_DIR)/mkfs_lufirafs
	@echo "=== Creating disk image ==="
	@rm -f $@ $(BUILD_DIR)/esp.img
	dd if=/dev/zero of=$@ bs=1024 count=$$(($(DISK_TOTAL_SIZE) / 1024)) status=none
	@echo "  Building ESP (FAT12, bootloader + kernel.bin only)..."
	dd if=/dev/zero of=$(BUILD_DIR)/esp.img bs=1024 count=$$(($(LUFIRAFS_ESP_SIZE) / 1024)) status=none
	mkfs.fat -F 12 -S 512 $(BUILD_DIR)/esp.img
	mmd -i $(BUILD_DIR)/esp.img ::/EFI
	mmd -i $(BUILD_DIR)/esp.img ::/EFI/BOOT
	mcopy -i $(BUILD_DIR)/esp.img $(BUILD_DIR)/BOOTX64.EFI ::/EFI/BOOT/BOOTX64.EFI
	mcopy -i $(BUILD_DIR)/esp.img $(BUILD_DIR)/kernel.bin ::/kernel.bin
	dd if=$(BUILD_DIR)/esp.img of=$@ conv=notrunc status=none
	rm -f $(BUILD_DIR)/esp.img
	@echo "  Formatting LufiraFS region..."
	$(BUILD_DIR)/mkfs_lufirafs format $@ $(LUFIRAFS_ESP_SIZE) $(LUFIRAFS_REGION_SIZE)
	@echo "  Populating initial files..."
	$(BUILD_DIR)/mkfs_lufirafs mkdir $@ $(LUFIRAFS_ESP_SIZE) $(LUFIRAFS_REGION_SIZE) /test
	echo "Hello from LufiraOS!" > $(BUILD_DIR)/readme.txt
	$(BUILD_DIR)/mkfs_lufirafs put $@ $(LUFIRAFS_ESP_SIZE) $(LUFIRAFS_REGION_SIZE) $(BUILD_DIR)/readme.txt /readme.txt
	rm -f $(BUILD_DIR)/readme.txt
	sync
	@echo "=== Disk image created: $@ ==="

check-disk: $(BUILD_DIR)/disk.img
	@echo "=== Checking disk image ==="
	@file $@

run: $(BUILD_DIR)/disk.img $(BUILD_DIR)/mkfs_lufirafs
	$(BUILD_DIR)/mkfs_lufirafs put $(BUILD_DIR)/disk.img $(LUFIRAFS_ESP_SIZE) $(LUFIRAFS_REGION_SIZE) test/hello.elf /hello.elf
	$(BUILD_DIR)/mkfs_lufirafs put $(BUILD_DIR)/disk.img $(LUFIRAFS_ESP_SIZE) $(LUFIRAFS_REGION_SIZE) test/fork_test.elf /fork.elf
	$(BUILD_DIR)/mkfs_lufirafs put $(BUILD_DIR)/disk.img $(LUFIRAFS_ESP_SIZE) $(LUFIRAFS_REGION_SIZE) test/pipe_test.elf /pipe.elf
	@echo "=== Starting QEMU ==="
	qemu-system-x86_64 \
		-bios /usr/share/ovmf/OVMF.fd \
		-drive file=$(BUILD_DIR)/disk.img,format=raw,if=ide,index=0 \
		-m 128M \
		-net none \
		-machine pcspk-audiodev=audio \
		-audiodev driver=alsa,id=audio \
		-device AC97,audiodev=audio \
		-device piix3-usb-uhci \
		-device usb-kbd \
		-device usb-mouse \
		-serial stdio

debug: $(BUILD_DIR)/disk.img
	qemu-system-x86_64 \
		-bios /usr/share/ovmf/OVMF.fd \
		-drive file=$(BUILD_DIR)/disk.img,format=raw,if=ide,index=0 \
		-m 256M -net none -serial stdio -no-reboot -no-shutdown \
		-device piix3-usb-uhci -device usb-kbd -device usb-mouse \
		-d cpu_reset,guest_errors -D $(BUILD_DIR)/qemu_debug.log

monitor: $(BUILD_DIR)/disk.img
	qemu-system-x86_64 \
		-bios /usr/share/ovmf/OVMF.fd \
		-drive file=$(BUILD_DIR)/disk.img,format=raw,if=ide,index=0 \
		-m 256M -net none -serial stdio \
		-device piix3-usb-uhci -device usb-kbd -device usb-mouse \
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