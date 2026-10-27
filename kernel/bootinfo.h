#pragma once

// Структура с информацией от загрузчика
// Передаётся ядру при старте
typedef struct {
    // Графика
    uint64_t FrameBufferBase;
    uint64_t FrameBufferSize;
    uint32_t HorizontalResolution;
    uint32_t VerticalResolution;
    uint32_t PixelsPerScanLine;
    uint32_t PixelFormat;           // 0 = RGB, 1 = BGR
    
    // Память
    uint64_t TotalMemory;
    uint64_t MemoryMapSize;
    void* MemoryMap;
    uint32_t MemoryMapDescriptorSize;
    
    // Ядро
    uint64_t KernelBase;
    uint64_t KernelSize;
    
    // Системные таблицы
    uint64_t RsdpAddress;           // ACPI RSDP
    uint64_t SmbiosAddress;         // SMBIOS
    
    // v1.0.0 план: больше НЕ используются (всегда 0) — LufiraFS теперь
    // читается напрямую с диска (lufirafs_init(), drivers/disk/mbr.c), а
    // не из предзагруженного бутлоадером в RAM образа. Поля оставлены
    // нулевыми, а не удалены — менять layout структуры без необходимости
    // рискованно (должна совпадать с boot/bootinfo.h байт-в-байт).
    uint64_t FATImageBase;
    uint64_t FATImageSize;
} BootInfo;

// Сигнатура точки входа ядра
typedef void (*KernelEntry)(BootInfo*);
