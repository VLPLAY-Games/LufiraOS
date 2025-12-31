#include <stdint.h>

typedef struct {
    uint64_t FrameBufferBase;
    uint64_t FrameBufferSize;
    uint32_t HorizontalResolution;
    uint32_t VerticalResolution;
    uint32_t PixelsPerScanLine;
} BootInfo;

void _start(BootInfo* bi) {
    uint32_t* fb = (uint32_t*)bi->FrameBufferBase;
    uint32_t color = 0x001e90ff; // DodgerBlue

    for (uint64_t i = 0; i < (bi->FrameBufferSize / 4); i++) {
        fb[i] = color;
    }

    uint32_t white = 0xffffffff;
    uint32_t size = 150;
    uint32_t start_x = (bi->HorizontalResolution / 2) - (size / 2);
    uint32_t start_y = (bi->VerticalResolution / 2) - (size / 2);

    for (uint32_t y = start_y; y < start_y + size; y++) {
        for (uint32_t x = start_x; x < start_x + size; x++) {
            fb[y * bi->PixelsPerScanLine + x] = white;
        }
    }

    while (1) {
        __asm__ volatile ("hlt");
    }
}
