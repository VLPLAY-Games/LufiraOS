// legacy_main.c — собирается и грузится как отдельный плоский бинарник,
// как kernel.bin (objcopy -O binary, точка входа — первый байт); почему
// не слинковано со stage2.asm — см. layout.inc. Исполняется уже в long
// mode (identity-карта 0..4GB от stage2.asm), стек настроен, прерывания
// выключены (stage2 держал cli с входа в protected mode).
//
// Задача — превратить LegacyHandoff (данные, доступные из 16-битного
// BIOS-кода) в такой же BootInfo, какой готовит boot/ (UEFI-путь), и
// передать управление ядру. Ядро не знает, каким путём оно загружено.

#include <stdint.h>
#include "legacy_boot.h"

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
    uint64_t FATImageBase;
    uint64_t FATImageSize;
} BootInfo;

typedef void (*KernelEntry)(BootInfo*);

// Тот же layout, что kernel/system/mm/pmm.h ожидает от UEFI-пути — см.
// подробный комментарий там. ПОРЯДОК И ТИПЫ ПОЛЕЙ менять нельзя.
typedef struct {
    uint32_t Type;
    uint64_t PhysicalStart;
    uint64_t VirtualStart;
    uint64_t NumberOfPages;
    uint64_t Attribute;
} EFI_MEMORY_DESCRIPTOR;

#define EFI_CONVENTIONAL_MEMORY 7u
#define EFI_RESERVED_MEMORY     0u

// Сырая запись E820 (INT15h, EAX=E820h) — 20 или 24 байта, см. do_e820()
// в stage2.asm. ExtendedAttributes читаем лениво (не нужен для конвертации).
typedef struct __attribute__((packed)) {
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t ext_attributes;
} E820Entry;

#define E820_TYPE_USABLE 1u

// Буфер конвертированных дескрипторов — статический массив (malloc ещё
// нет). 256 записей с запасом: реальные BIOS обычно отдают 10-20.
#define MAX_MEMORY_DESCRIPTORS 256
static EFI_MEMORY_DESCRIPTOR g_memmap[MAX_MEMORY_DESCRIPTORS];

static void* memset_local(void* dst, int v, uint64_t n) {
    uint8_t* p = (uint8_t*)dst;
    for (uint64_t i = 0; i < n; i++) p[i] = (uint8_t)v;
    return dst;
}

static uint32_t convert_e820(const E820Entry* src, uint32_t count) {
    uint32_t out = 0;
    for (uint32_t i = 0; i < count && out < MAX_MEMORY_DESCRIPTORS; i++) {
        if (src[i].length == 0) continue;
        g_memmap[out].Type = (src[i].type == E820_TYPE_USABLE)
                                  ? EFI_CONVENTIONAL_MEMORY : EFI_RESERVED_MEMORY;
        g_memmap[out].PhysicalStart = src[i].base;
        g_memmap[out].VirtualStart = src[i].base;
        g_memmap[out].NumberOfPages = src[i].length / 4096u; // консервативно, в пол
        g_memmap[out].Attribute = 0;
        if (g_memmap[out].NumberOfPages == 0) continue;
        out++;
    }
    return out;
}

// Сканирует [start,end) с шагом 16 байт на сигнатуру sig (len байт).
// Возвращает физический адрес найденного совпадения или 0.
static uint64_t scan_signature(uint64_t start, uint64_t end, const char* sig, uint32_t len) {
    for (uint64_t addr = start; addr + len <= end; addr += 16) {
        const uint8_t* p = (const uint8_t*)addr;
        uint32_t i = 0;
        for (; i < len; i++) {
            if (p[i] != (uint8_t)sig[i]) break;
        }
        if (i == len) return addr;
    }
    return 0;
}

static uint64_t find_rsdp(void) {
    // EBDA: сегмент хранится как uint16 по физическому адресу 0x40E
    // (BIOS Data Area). База EBDA = значение*16. Если BIOS его не
    // заполнил (0), пропускаем этот диапазон — ниже всё равно просканируем
    // весь 0xE0000..0xFFFFF, где RSDP лежит почти всегда.
    uint16_t ebda_seg = *(volatile uint16_t*)(uint64_t)0x40E;
    if (ebda_seg != 0) {
        uint64_t ebda_base = (uint64_t)ebda_seg << 4;
        uint64_t found = scan_signature(ebda_base, ebda_base + 1024, "RSD PTR ", 8);
        if (found) return found;
    }
    return scan_signature(0xE0000, 0x100000, "RSD PTR ", 8);
}

static uint64_t find_smbios(void) {
    uint64_t found = scan_signature(0xF0000, 0x100000, "_SM3_", 5);
    if (found) return found;
    return scan_signature(0xF0000, 0x100000, "_SM_", 4);
}

__attribute__((noreturn))
static void halt_forever(void) {
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}

__attribute__((section(".text.prologue")))
void legacy_main(LegacyHandoff* h) {
    static BootInfo bi;
    memset_local(&bi, 0, sizeof(bi));

    bi.FrameBufferBase = h->fb_base;
    bi.HorizontalResolution = h->fb_width;
    bi.VerticalResolution = h->fb_height;
    bi.PixelsPerScanLine = h->fb_pitch / 4u; // framebuffer всегда 32bpp (см. do_vesa в stage2.asm)
    bi.FrameBufferSize = (uint64_t)h->fb_pitch * h->fb_height;
    bi.PixelFormat = (h->fb_red_field_pos == 16) ? 1u : 0u;

    E820Entry* e820 = (E820Entry*)h->e820_addr;
    uint32_t desc_count = convert_e820(e820, h->e820_count);
    bi.MemoryMap = g_memmap;
    bi.MemoryMapDescriptorSize = sizeof(EFI_MEMORY_DESCRIPTOR);
    bi.MemoryMapSize = (uint64_t)desc_count * sizeof(EFI_MEMORY_DESCRIPTOR);

    uint64_t total = 0;
    for (uint32_t i = 0; i < desc_count; i++) {
        if (g_memmap[i].Type == EFI_CONVENTIONAL_MEMORY)
            total += g_memmap[i].NumberOfPages * 4096u;
    }
    bi.TotalMemory = total;

    bi.KernelBase = h->kernel_base;
    bi.KernelSize = h->kernel_size;
    bi.FATImageBase = h->fatimage_base;
    bi.FATImageSize = h->fatimage_size;

    bi.RsdpAddress = find_rsdp();
    bi.SmbiosAddress = find_smbios();

    if (bi.KernelBase == 0) {
        halt_forever();
    }

    KernelEntry entry = (KernelEntry)bi.KernelBase;
    entry(&bi);

    halt_forever(); // ядро не должно сюда вернуться — страховка
}
