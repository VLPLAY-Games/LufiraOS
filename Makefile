# Makefile для сборки SimpleOS

# Компиляторы и утилиты
ASM = nasm
CC = gcc
LD = ld
OBJCOPY = objcopy

# Флаги компиляции
ASM_FLAGS = -f elf32
CC_FLAGS = -m32 -std=gnu99 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector -O2 -Wall -Wextra
LD_FLAGS = -m elf_i386 -T linker.ld -nostdlib

# Имена файлов
BOOTLOADER = boot.bin
KERNEL = kernel.bin
OS_IMAGE = simpleos.img

# Цель по умолчанию
all: $(OS_IMAGE)

# Сборка загрузчика
$(BOOTLOADER): boot.asm
	$(ASM) -f bin boot.asm -o $(BOOTLOADER)

# Компиляция точки входа ядра
kernel_entry.o: kernel_entry.asm
	$(ASM) $(ASM_FLAGS) kernel_entry.asm -o kernel_entry.o

# Компиляция ядра на C
kernel.o: kernel.c
	$(CC) $(CC_FLAGS) -c kernel.c -o kernel.o

# Линковка ядра
kernel.elf: kernel_entry.o kernel.o
	$(LD) $(LD_FLAGS) -o kernel.elf kernel_entry.o kernel.o

# Конвертация в бинарный формат
kernel.bin: kernel.elf
	$(OBJCOPY) -O binary kernel.elf $(KERNEL)

# Создание образа диска
$(OS_IMAGE): $(BOOTLOADER) $(KERNEL)
	# Создаем пустой образ 1.44MB (2880 секторов по 512 байт)
	dd if=/dev/zero of=$(OS_IMAGE) bs=512 count=2880
	
	# Копируем загрузчик в начало
	dd if=$(BOOTLOADER) of=$(OS_IMAGE) conv=notrunc
	
	# Копируем ядро, начиная со второго сектора
	dd if=$(KERNEL) of=$(OS_IMAGE) conv=notrunc bs=512 seek=1

# Запуск в QEMU
run: $(OS_IMAGE)
	qemu-system-i386 -fda $(OS_IMAGE)

# Запуск с отладкой
debug: $(OS_IMAGE)
	qemu-system-i386 -S -s -fda $(OS_IMAGE) &
	gdb -ex "target remote localhost:1234" -ex "symbol-file kernel.elf"

# Запуск с монитором
monitor: $(OS_IMAGE)
	qemu-system-i386 -fda $(OS_IMAGE) -monitor stdio

# Очистка
clean:
	rm -f *.o *.bin *.elf *.img

.PHONY: all run debug monitor clean