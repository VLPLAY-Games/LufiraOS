#ifndef BOOTINFO_H
#define BOOTINFO_H

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
    
    // Файловая система
    uint64_t FATImageBase;          // Адрес загруженного FAT образа в памяти
    uint64_t FATImageSize;          // Размер FAT образа в байтах
} BootInfo;

// Сигнатура точки входа ядра
typedef void (*KernelEntry)(BootInfo*);

#endif // BOOTINFO_H