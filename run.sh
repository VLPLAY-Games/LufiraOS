#!/bin/bash
echo "Building LufiraOS..."

# Создаем структуру каталогов
mkdir -p build/iso/EFI/BOOT

# Компилируем
gcc -ffreestanding -fpic -fshort-wchar -mno-red-zone \
    -I/usr/include/efi -I/usr/include/efi/x86_64 -I./src/include \
    -c src/boot/main.c -o build/boot.o

gcc -ffreestanding -fpic -fshort-wchar -mno-red-zone \
    -I/usr/include/efi -I/usr/include/efi/x86_64 -I./src/include \
    -c src/kernel/kernel.c -o build/kernel.o

# Линкуем
ld -nostdlib -T /usr/lib/elf_x86_64_efi.lds -shared -Bsymbolic \
    /usr/lib/crt0-efi-x86_64.o build/boot.o build/kernel.o \
    -o build/main.elf -lefi -lgnuefi

# Конвертируем в EFI
objcopy -j .text -j .sdata -j .data -j .rodata \
        -j .dynamic -j .dynsym -j .rel* \
        --target=efi-app-x86_64 build/main.elf build/BOOTX64.EFI

# Копируем в структуру ISO
cp build/BOOTX64.EFI build/iso/EFI/BOOT/

# Создаем ISO
dd if=/dev/zero of=build/fat.img bs=1k count=1440
mformat -i build/fat.img -f 1440 ::
mmd -i build/fat.img ::/EFI ::/EFI/BOOT
mcopy -i build/fat.img build/iso/EFI/BOOT/BOOTX64.EFI ::/EFI/BOOT
xorriso -as mkisofs -b fat.img -no-emul-boot -o build/lufiraos.iso build

echo "Build complete! Run with: qemu-system-x86_64 -bios /usr/share/ovmf/OVMF.fd -cdrom build/lufiraos.iso"