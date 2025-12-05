#!/bin/bash
# run.sh - скрипт для быстрого запуска LufiraOS

echo "====================================="
echo "       LufiraOS Build and Run"
echo "====================================="
echo ""

# Убедиться, что у нас есть необходимые инструменты
echo "[1/3] Checking dependencies..."
if ! command -v nasm &> /dev/null; then
    echo "ERROR: NASM not found! Install with: sudo apt install nasm"
    exit 1
fi

if ! command -v gcc &> /dev/null; then
    echo "ERROR: GCC not found!"
    exit 1
fi

if ! command -v qemu-system-i386 &> /dev/null; then
    echo "ERROR: QEMU not found! Install with: sudo apt install qemu-system-x86"
    exit 1
fi

# Сборка
echo "[2/3] Building LufiraOS..."
make clean
make || exit 1

# Запуск
echo "[3/3] Starting QEMU..."
echo ""
echo "====================================="
echo "   Press Ctrl+Alt+G to release mouse"
echo "   Press Ctrl+Alt+Del to restart"
echo "   Press Ctrl+Alt to exit QEMU"
echo "====================================="
echo ""

qemu-system-i386 -fda build/lufiraos.img -no-reboot -no-shutdown