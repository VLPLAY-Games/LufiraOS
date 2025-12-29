# Makefile для сборки LufiraOS

# Компиляторы и утилиты
ASM = nasm
CC = gcc
LD = ld
OBJCOPY = objcopy

# Флаги компиляции
ASM_FLAGS = -f elf32
CC_FLAGS = -m32 -std=gnu99 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector -O0 -Wall -Wextra -I./kernel -I./lib
LD_FLAGS = -m elf_i386 -T linker.ld -nostdlib

# Имена файлов
BOOTLOADER = build/boot.bin
STAGE2 = build/stage2.bin
KERNEL = build/kernel.bin
OS_IMAGE = build/lufiraos.img

# Цели сборки
BOOT_OBJ = boot/boot.asm
KERNEL_OBJS = build/kernel_entry.o build/kernel.o build/keyboard.o build/shell.o build/string.o build/fs.o build/disk.o build/memory.o

# Создание директории build
$(shell mkdir -p build)

# Цель по умолчанию
all: $(OS_IMAGE)

# Сборка загрузчика
$(BOOTLOADER): $(BOOT_OBJ)
	$(ASM) -f bin $< -o $@

$(STAGE2): boot/stage2.asm
	$(ASM) -f bin $< -o $@

# Компиляция точки входа ядра
build/kernel_entry.o: kernel/kernel_entry.asm
	$(ASM) $(ASM_FLAGS) $< -o $@

# Компиляция ядра на C
build/kernel.o: kernel/kernel.c drivers/keyboard.h kernel/shell.h lib/string.h
	$(CC) $(CC_FLAGS) -c kernel/kernel.c -o $@

build/keyboard.o: drivers/keyboard.c drivers/keyboard.h
	$(CC) $(CC_FLAGS) -c drivers/keyboard.c -o $@

build/shell.o: kernel/shell.c kernel/shell.h drivers/keyboard.h lib/string.h
	$(CC) $(CC_FLAGS) -c kernel/shell.c -o $@

build/fs.o: fs/fs.c fs/fs.h lib/string.h
	$(CC) $(CC_FLAGS) -c fs/fs.c -o $@

build/disk.o: fs/disk.c fs/disk.h
	$(CC) $(CC_FLAGS) -c fs/disk.c -o $@

# Компиляция библиотек
build/string.o: lib/string.c lib/string.h
	$(CC) $(CC_FLAGS) -c lib/string.c -o $@

build/memory.o: kernel/memory.c kernel/memory.h 
	$(CC) $(CC_FLAGS) -c kernel/memory.c -o $@

# Линковка ядра
build/kernel.elf: $(KERNEL_OBJS)
	$(LD) $(LD_FLAGS) -o build/kernel.elf $(KERNEL_OBJS)
	@echo "Kernel size: $$(stat -c%s build/kernel.elf) bytes"

# Конвертация в бинарный формат
$(KERNEL): build/kernel.elf
	$(OBJCOPY) -O binary build/kernel.elf $(KERNEL)
	@echo "Kernel binary size: $$(stat -c%s $(KERNEL)) bytes"

# Создание образа жесткого диска
$(OS_IMAGE): $(BOOTLOADER) $(STAGE2) $(KERNEL)
	@echo "Creating hard disk image..."
	# Создаем образ жесткого диска 2MB
	dd if=/dev/zero of=$(OS_IMAGE) bs=1M count=2 2>/dev/null
	# Создаем таблицу разделов и копируем загрузчик
	dd if=$(BOOTLOADER) of=$(OS_IMAGE) conv=notrunc 2>/dev/null
	# Копируем Stage 2 (начиная с сектора 1)
	dd if=$(STAGE2) of=$(OS_IMAGE) conv=notrunc bs=512 seek=1 2>/dev/null
	# Копируем ядро (начиная с сектора 34)
	dd if=$(KERNEL) of=$(OS_IMAGE) conv=notrunc bs=512 seek=34 2>/dev/null
	@echo "Hard disk image created: $(OS_IMAGE)"

# Запуск в QEMU через жесткий диск
run: $(OS_IMAGE)
	@echo "Starting QEMU with hard disk..."
	qemu-system-x86_64 -hda $(OS_IMAGE) -no-reboot

# Запуск через жесткий диск с отладкой
debug: $(OS_IMAGE)
	@echo "Starting QEMU in debug mode (hard disk)..."
	qemu-system-x86_64 -S -s -hda $(OS_IMAGE) -no-reboot &
	@echo "Waiting for GDB connection..."
	@sleep 1
	gdb -ex "target remote localhost:1234" -ex "symbol-file build/kernel.elf" -ex "break _start" -ex "continue"

# Очистка
clean:
	rm -rf build/*

.PHONY: all run debug clean