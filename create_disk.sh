#!/bin/bash
# create_disk.sh - Альтернативный метод создания диска

BUILD_DIR=$1

if [ -z "$BUILD_DIR" ]; then
    echo "Usage: $0 <build_directory>"
    exit 1
fi

DISK_IMG="$BUILD_DIR/disk.img"
BOOTLOADER="$BUILD_DIR/BOOTX64.EFI"
KERNEL="$BUILD_DIR/kernel.bin"

# Создаем пустой образ 64MB
echo "Creating empty disk image..."
dd if=/dev/zero of="$DISK_IMG" bs=1M count=64

# Создаем раздел и форматируем
echo "Partitioning and formatting..."
printf "g\nn\n\n\n\nt\n1\nw\n" | fdisk "$DISK_IMG" 2>/dev/null

# Используем losetup для монтирования
LOOP_DEV=$(sudo losetup -f --show "$DISK_IMG")
PART_DEV="${LOOP_DEV}p1"

# Создаем раздел в loop устройстве
sudo partprobe "$LOOP_DEV"

# Форматируем как FAT32
sudo mkfs.fat -F 32 -S 512 "$PART_DEV"

# Монтируем
MNT_DIR=$(mktemp -d)
sudo mount "$PART_DEV" "$MNT_DIR"

# Создаем структуру EFI
sudo mkdir -p "$MNT_DIR/EFI/BOOT"

# Копируем файлы
sudo cp "$BOOTLOADER" "$MNT_DIR/EFI/BOOT/BOOTX64.EFI"
sudo cp "$KERNEL" "$MNT_DIR/kernel.bin"

# Размонтируем
sudo umount "$MNT_DIR"
sudo losetup -d "$LOOP_DEV"
rmdir "$MNT_DIR"

echo "Disk image created: $DISK_IMG"
echo "Contents:"
mdir -i "$DISK_IMG" ::/EFI/BOOT/
mdir -i "$DISK_IMG" ::/