#pragma once

#include <efi.h>

typedef struct {
    uint64_t FrameBufferBase;
    uint64_t FrameBufferSize;
    uint32_t HorizontalResolution;
    uint32_t VerticalResolution;
    uint32_t PixelsPerScanLine;
    uint32_t PixelFormat;
    uint64_t TotalMemory;
    uint64_t MemoryMapSize;
    void* MemoryMap;
    uint32_t MemoryMapDescriptorSize;
    uint64_t KernelBase;
    uint64_t KernelSize;
    uint64_t RsdpAddress;
    uint64_t SmbiosAddress;
    // v1.0.0 план: больше НЕ используются (всегда 0, см. boot_modes/*.c) —
    // LufiraFS читается напрямую с диска ядром, не из предзагруженного
    // образа. Оставлены нулевыми, не удалены — layout структуры должен
    // совпадать с kernel/bootinfo.h байт-в-байт.
    uint64_t FATImageBase;
    uint64_t FATImageSize;
} BootInfo;

typedef void (*KernelEntry)(BootInfo*);

typedef enum { MODE_NORMAL, MODE_DEBUG, MODE_SAFE } BootMode;